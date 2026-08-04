#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "cdn_types.hpp"
#include "config.hpp"
#include "geoip.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::atomic<int> active_connections{0};
std::atomic<unsigned long long> total_requests{0};
std::atomic<unsigned long long> total_errors{0};
std::atomic<unsigned long long> total_failovers{0};
std::mutex log_mutex;

struct WorkItem {
    int fd;
    sockaddr_storage peer;
};

std::queue<WorkItem> work_queue;
std::mutex work_mutex;
std::condition_variable work_available;
volatile std::sig_atomic_t stopping = 0;
int listening_socket = -1;

void request_shutdown(int) {
    stopping = 1;
    if (listening_socket >= 0) close(listening_socket);
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string normalize_location(const std::string& value) {
    std::string normalized;
    for (unsigned char c : value) if (std::isalnum(c)) normalized += static_cast<char>(std::tolower(c));
    return normalized;
}

std::string india_region_for(const std::string& location) {
    static const std::unordered_map<std::string, std::string> regions = {
        {"chandigarh","north"},{"delhi","north"},{"nationalcapitalterritoryofdelhi","north"},{"haryana","north"},
        {"himachalpradesh","north"},{"jammukashmir","north"},{"ladakh","north"},{"punjab","north"},
        {"rajasthan","north"},{"uttarpradesh","north"},{"uttarakhand","north"},
        {"chhattisgarh","west-central"},{"dadraandnagarhavelianddamandiu","west-central"},{"goa","west-central"},
        {"gujarat","west-central"},{"madhyapradesh","west-central"},{"maharashtra","west-central"},
        {"andhrapradesh","south-east"},{"arunachalpradesh","south-east"},{"assam","south-east"},{"bihar","south-east"},
        {"jharkhand","south-east"},{"karnataka","south-east"},{"kerala","south-east"},{"manipur","south-east"},
        {"meghalaya","south-east"},{"mizoram","south-east"},{"nagaland","south-east"},{"odisha","south-east"},
        {"puducherry","south-east"},{"sikkim","south-east"},{"tamilnadu","south-east"},{"telangana","south-east"},
        {"tripura","south-east"},{"westbengal","south-east"},{"andamanandnicobarislands","south-east"},{"lakshadweep","south-east"}
    };
    const auto found = regions.find(normalize_location(location));
    return found == regions.end() ? "unknown" : found->second;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
        else out << c;
    }
    return out.str();
}

void log_event(const std::string& path, const std::string& region, const std::string& source, int status, long duration_ms) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << "{\"path\":\"" << json_escape(path) << "\",\"region\":\"" << json_escape(region)
              << "\",\"source\":\"" << source << "\",\"status\":" << status
              << ",\"duration_ms\":" << duration_ms << "}\n" << std::flush;
}

bool parse_int(const std::string& text, int minimum, int maximum, int& result) {
    try {
        size_t used = 0;
        const long value = std::stol(text, &used);
        if (used != text.size() || value < minimum || value > maximum) return false;
        result = static_cast<int>(value); return true;
    } catch (...) { return false; }
}


bool send_all(int socket_fd, const char* data, size_t size) {
    while (size) {
        const ssize_t sent = send(socket_fd, data, size, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += sent; size -= static_cast<size_t>(sent);
    }
    return true;
}
bool send_all(int fd, const std::string& data) { return send_all(fd, data.data(), data.size()); }

void set_timeout(int fd, int milliseconds) {
    timeval value{milliseconds / 1000, (milliseconds % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
}

std::optional<Request> read_request(int fd, int& status) {
    std::string raw; std::array<char, 4096> chunk{};
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const ssize_t received = recv(fd, chunk.data(), chunk.size(), 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) { status = 408; return std::nullopt; }
        raw.append(chunk.data(), static_cast<size_t>(received));
        if (raw.size() > 32768) { status = 431; return std::nullopt; }
    }
    Request result;
    std::istringstream stream(raw); std::string line;
    if (!std::getline(stream, line)) { status = 400; return std::nullopt; }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream first(line); std::string extra;
    if (!(first >> result.method >> result.target >> result.version) || (first >> extra) || result.version.rfind("HTTP/1.", 0) != 0) { status = 400; return std::nullopt; }
    while (std::getline(stream, line) && line != "\r") {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) { status = 400; return std::nullopt; }
        result.headers.emplace_back(lower(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
    }
    status = 200; return result;
}

std::string header(const Request& request, const std::string& name) {
    for (const auto& item : request.headers) if (item.first == name) return item.second;
    return {};
}

std::string status_text(int status) {
    static const std::map<int,std::string> values = {{200,"OK"},{206,"Partial Content"},{304,"Not Modified"},{400,"Bad Request"},{403,"Forbidden"},{404,"Not Found"},
        {405,"Method Not Allowed"},{408,"Request Timeout"},{413,"Payload Too Large"},{431,"Request Header Fields Too Large"},
        {416,"Range Not Satisfiable"},{500,"Internal Server Error"},{502,"Bad Gateway"},{503,"Service Unavailable"}};
    const auto found = values.find(status); return found == values.end() ? "Error" : found->second;
}

void reply(int fd, int status, const std::string& body, const std::string& type, bool include_body, const Config& config,
           const std::vector<std::pair<std::string,std::string>>& extra = {}) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << status_text(status) << "\r\nContent-Type: " << type
             << "\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n"
             << "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nPermissions-Policy: geolocation=(self)\r\n"
             << "Content-Security-Policy: default-src 'self'; style-src 'self' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\n"
             << "Access-Control-Allow-Origin: *\r\nX-CDN-Edge-Region: " << config.region << "\r\n";
    for (const auto& item : extra) response << item.first << ": " << item.second << "\r\n";
    response << "\r\n"; send_all(fd, response.str()); if (include_body) send_all(fd, body);
}

std::string url_decode(const std::string& text, bool& valid) {
    valid = true; std::string result;
    auto hex=[](char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; };
    for(size_t i=0;i<text.size();++i){ if(text[i]=='%'){ if(i+2>=text.size()){valid=false;return{};} int a=hex(text[i+1]),b=hex(text[i+2]); if(a<0||b<0){valid=false;return{};} char c=static_cast<char>((a<<4)|b); if(!c){valid=false;return{};} result+=c;i+=2;} else result+=text[i]; }
    return result;
}

bool is_inside(const fs::path& child, const fs::path& parent) {
    auto c=child.begin(); for(auto p=parent.begin();p!=parent.end();++p,++c) if(c==child.end()||*c!=*p)return false; return true;
}

std::string content_type(const fs::path& path) {
    static const std::unordered_map<std::string,std::string> types={{".html","text/html; charset=utf-8"},{".css","text/css; charset=utf-8"},{".js","application/javascript; charset=utf-8"},{".json","application/json; charset=utf-8"},{".svg","image/svg+xml"},{".png","image/png"},{".jpg","image/jpeg"},{".jpeg","image/jpeg"},{".gif","image/gif"},{".webp","image/webp"},{".ico","image/x-icon"},{".pdf","application/pdf"},{".woff","font/woff"},{".woff2","font/woff2"},{".mp4","video/mp4"},{".webm","video/webm"},{".txt","text/plain; charset=utf-8"}};
    const auto found=types.find(lower(path.extension().string())); return found==types.end()?"application/octet-stream":found->second;
}

int connect_to(const Endpoint& endpoint, int timeout_ms) {
    addrinfo hints{}, *addresses=nullptr; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(endpoint.host.c_str(),std::to_string(endpoint.port).c_str(),&hints,&addresses)!=0)return -1;
    int fd=-1;
    for(auto* address=addresses;address;address=address->ai_next){
        fd=socket(address->ai_family,address->ai_socktype,address->ai_protocol);if(fd<0)continue;
        const int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){close(fd);fd=-1;continue;}
        const int result=connect(fd,address->ai_addr,address->ai_addrlen);
        bool connected=result==0;
        if(!connected&&errno==EINPROGRESS){pollfd descriptor{fd,POLLOUT,0};if(poll(&descriptor,1,timeout_ms)>0){int socket_error=0;socklen_t length=sizeof(socket_error);connected=getsockopt(fd,SOL_SOCKET,SO_ERROR,&socket_error,&length)==0&&socket_error==0;}}
        if(connected){fcntl(fd,F_SETFL,flags);set_timeout(fd,timeout_ms);break;}
        close(fd);fd=-1;
    }
    freeaddrinfo(addresses); return fd;
}

int response_status(const std::string& response) {
    std::istringstream first_line(response.substr(0, response.find("\r\n")));
    std::string version;
    int status = 0;
    return (first_line >> version >> status) && version.rfind("HTTP/1.", 0) == 0 ? status : 0;
}

bool edge_is_healthy(const Endpoint& endpoint, const Config& config) {
    const int fd = connect_to(endpoint, std::min(config.timeout_ms, 1000));
    if (fd < 0) return false;
    std::ostringstream request;
    request << "GET /health HTTP/1.1\r\nHost: " << endpoint.host << "\r\nConnection: close\r\n";
    if (!config.router_token.empty()) request << "X-CDN-Router-Token: " << config.router_token << "\r\n";
    request << "\r\n";
    std::array<char, 4096> response{};
    const bool sent = send_all(fd, request.str());
    const ssize_t received = sent ? recv(fd, response.data(), response.size(), 0) : -1;
    close(fd);
    return received > 0 && response_status(std::string(response.data(), static_cast<size_t>(received))) == 200;
}

struct EdgeHealthSnapshot {
    bool healthy = false;
    int consecutive_failures = 0;
    int consecutive_successes = 0;
};

class HealthMonitor {
public:
    void start(const Config& config) {
        if (running_) return;
        config_ = &config;
        for (const auto& edge : config.edges) states_[edge.first] = {};
        probe_all(true);
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            running_ = false;
        }
        wait_condition_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    bool healthy(const std::string& region) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto found = states_.find(region);
        return found != states_.end() && found->second.healthy;
    }

    std::map<std::string, EdgeHealthSnapshot> snapshot() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return states_;
    }

private:
    void run() {
        while (true) {
            std::unique_lock<std::mutex> lock(wait_mutex_);
            if (wait_condition_.wait_for(lock, std::chrono::milliseconds(config_->health_interval_ms),
                                         [this] { return !running_; })) return;
            lock.unlock();
            probe_all(false);
        }
    }

    void probe_all(bool initial) {
        for (const auto& edge : config_->edges) {
            const bool succeeded = edge_is_healthy(edge.second, *config_);
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto& state = states_[edge.first];
            if (initial) {
                state.healthy = succeeded;
                state.consecutive_successes = succeeded ? 1 : 0;
                state.consecutive_failures = succeeded ? 0 : 1;
            } else if (succeeded) {
                state.consecutive_failures = 0;
                state.consecutive_successes++;
                if (!state.healthy && state.consecutive_successes >= config_->health_recovery_threshold) state.healthy = true;
            } else {
                state.consecutive_successes = 0;
                state.consecutive_failures++;
                if (state.healthy && state.consecutive_failures >= config_->health_failure_threshold) state.healthy = false;
            }
        }
    }

    const Config* config_ = nullptr;
    mutable std::mutex state_mutex_;
    std::map<std::string, EdgeHealthSnapshot> states_;
    std::mutex wait_mutex_;
    std::condition_variable wait_condition_;
    bool running_ = false;
    std::thread thread_;
};

HealthMonitor health_monitor;

bool ipv4_in_cidr(const std::string& ip, const std::string& cidr) {
    const auto slash=cidr.find('/'); std::string base=cidr.substr(0,slash); int bits=32;
    if(slash!=std::string::npos&&!parse_int(cidr.substr(slash+1),0,32,bits))return false;
    in_addr a{},b{}; if(inet_pton(AF_INET,ip.c_str(),&a)!=1||inet_pton(AF_INET,base.c_str(),&b)!=1)return false;
    const uint32_t mask=bits==0?0:htonl(0xffffffffu<<(32-bits)); return (a.s_addr&mask)==(b.s_addr&mask);
}

std::string peer_ip(const sockaddr_storage& peer) {
    char buffer[INET6_ADDRSTRLEN]{};
    if(peer.ss_family==AF_INET)inet_ntop(AF_INET,&reinterpret_cast<const sockaddr_in*>(&peer)->sin_addr,buffer,sizeof(buffer));
    else if(peer.ss_family==AF_INET6)inet_ntop(AF_INET6,&reinterpret_cast<const sockaddr_in6*>(&peer)->sin6_addr,buffer,sizeof(buffer));
    return buffer;
}

int serve_edge(int fd, const Request& request, const Config& config) {
    const bool head=request.method=="HEAD";
    if(request.method!="GET"&&!head){ reply(fd,405,"Only GET and HEAD are supported\n","text/plain; charset=utf-8",true,config);return 405; }
    if(!config.router_token.empty() && header(request,"x-cdn-router-token")!=config.router_token){ reply(fd,403,"Edge accepts requests from its router only\n","text/plain; charset=utf-8",!head,config);return 403; }
    std::string target=request.target; const auto suffix=target.find_first_of("?#"); if(suffix!=std::string::npos)target.erase(suffix);
    if(target=="/health"){ reply(fd,200,"{\"status\":\"ok\",\"region\":\""+json_escape(config.region)+"\"}\n","application/json",!head,config);return 200; }
    if(target.empty()||target.front()!='/'){reply(fd,400,"Malformed request path\n","text/plain; charset=utf-8",!head,config);return 400;}
    bool valid=false; const std::string decoded=url_decode(target,valid);
    if(!valid||decoded.find('\\')!=std::string::npos){reply(fd,400,"Invalid request path\n","text/plain; charset=utf-8",!head,config);return 400;}
    try {
        fs::path relative=decoded.substr(1);if(relative.empty())relative="index.html";const fs::path candidate=fs::weakly_canonical(config.root/relative);
        if(!is_inside(candidate,config.root)){reply(fd,403,"Access denied\n","text/plain; charset=utf-8",!head,config);return 403;}
        if(!fs::is_regular_file(candidate)){reply(fd,404,"Asset not found\n","text/plain; charset=utf-8",!head,config);return 404;}
        const auto size=fs::file_size(candidate);
        const auto modified=fs::last_write_time(candidate).time_since_epoch().count();
        std::ostringstream etag_builder;etag_builder<<'"'<<std::hex<<size<<'-'<<static_cast<long long>(modified)<<'"';const std::string etag=etag_builder.str();
        if(header(request,"if-none-match")==etag){reply(fd,304,"",content_type(candidate),false,config,{{"ETag",etag},{"Cache-Control","public, max-age="+std::to_string(config.cache_seconds)}});return 304;}

        uintmax_t start=0,end=size?size-1:0;int response_status_code=200;const std::string range=header(request,"range");
        if(!range.empty()){
            bool valid_range=range.rfind("bytes=",0)==0&&range.find(',')==std::string::npos&&size>0;
            const auto dash=range.find('-',6);
            try{
                if(!valid_range||dash==std::string::npos)throw std::invalid_argument("range");
                const std::string first=range.substr(6,dash-6),last=range.substr(dash+1);
                if(first.empty()){
                    const auto suffix=static_cast<uintmax_t>(std::stoull(last));if(suffix==0)throw std::invalid_argument("range");start=suffix>=size?0:size-suffix;
                }else{
                    start=static_cast<uintmax_t>(std::stoull(first));end=last.empty()?size-1:static_cast<uintmax_t>(std::stoull(last));
                }
                if(start>=size||end<start){throw std::invalid_argument("range");}end=std::min(end,size-1);response_status_code=206;
            }catch(...){reply(fd,416,"Requested range is unavailable\n","text/plain; charset=utf-8",!head,config,{{"Content-Range","bytes */"+std::to_string(size)},{"Accept-Ranges","bytes"},{"ETag",etag}});return 416;}
        }
        const uintmax_t response_size=size?end-start+1:0;std::ostringstream headers;
        headers<<"HTTP/1.1 "<<response_status_code<<' '<<status_text(response_status_code)<<"\r\nContent-Type: "<<content_type(candidate)<<"\r\nContent-Length: "<<response_size<<"\r\nCache-Control: public, max-age="<<config.cache_seconds<<"\r\nETag: "<<etag<<"\r\nAccept-Ranges: bytes\r\n";
        if(response_status_code==206)headers<<"Content-Range: bytes "<<start<<'-'<<end<<'/'<<size<<"\r\n";
        headers<<"Access-Control-Allow-Origin: *\r\nX-CDN-Edge-Region: "<<config.region<<"\r\nX-CDN-Decision-Source: "<<header(request,"x-cdn-decision-source")<<"\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nPermissions-Policy: geolocation=(self)\r\nContent-Security-Policy: default-src 'self'; style-src 'self' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\nConnection: close\r\n\r\n";
        if(send_all(fd,headers.str())&&!head&&response_size>0){std::ifstream file(candidate,std::ios::binary);file.seekg(static_cast<std::streamoff>(start));std::array<char,65536> chunk{};uintmax_t remaining=response_size;while(file&&remaining>0){const auto wanted=static_cast<std::streamsize>(std::min<uintmax_t>(chunk.size(),remaining));file.read(chunk.data(),wanted);const auto count=file.gcount();if(count<=0||!send_all(fd,chunk.data(),static_cast<size_t>(count)))break;remaining-=static_cast<uintmax_t>(count);}}
        return response_status_code;
    } catch(const fs::filesystem_error&){reply(fd,500,"Unable to read asset\n","text/plain; charset=utf-8",!head,config);return 500;}
}

std::string query_value(const std::string& target, const std::string& key) {
    const auto question=target.find('?'); if(question==std::string::npos)return{}; std::istringstream parts(target.substr(question+1));std::string part;
    while(std::getline(parts,part,'&')){const auto equals=part.find('=');if(part.substr(0,equals)==key){bool valid=false;return url_decode(equals==std::string::npos?"":part.substr(equals+1),valid);}}
    return{};
}

int proxy_request(int client, const Request& request, const Config& config, const std::string& region, const std::string& visitor_ip, const std::string& source, std::string& served_region) {
    std::vector<std::string> order;if(health_monitor.healthy(region))order.push_back(region);for(const auto& edge:config.edges)if(edge.first!=region&&health_monitor.healthy(edge.first))order.push_back(edge.first);
    for(const auto& name:order){
        const int upstream=connect_to(config.edges.at(name),config.timeout_ms);if(upstream<0)continue;
        std::ostringstream forwarded;forwarded<<request.method<<' '<<request.target<<" HTTP/1.1\r\nHost: "<<config.edges.at(name).host<<"\r\nConnection: close\r\nX-Forwarded-For: "<<visitor_ip<<"\r\nX-CDN-Decision-Source: "<<source<<"\r\n";
        if(!config.router_token.empty())forwarded<<"X-CDN-Router-Token: "<<config.router_token<<"\r\n";
        for(const auto& h:request.headers)if(h.first!="host"&&h.first!="connection"&&h.first!="x-forwarded-for"&&h.first!="x-cdn-test-region"&&h.first!="x-cdn-router-token")forwarded<<h.first<<": "<<h.second<<"\r\n";
        forwarded<<"\r\n";if(!send_all(upstream,forwarded.str())){close(upstream);continue;}
        std::array<char,65536> buffer{};ssize_t count=recv(upstream,buffer.data(),buffer.size(),0);if(count<=0){close(upstream);continue;}
        const std::string first(buffer.data(),static_cast<size_t>(count));const int upstream_status=response_status(first);if(upstream_status==0){close(upstream);continue;}
        send_all(client,buffer.data(),static_cast<size_t>(count));while((count=recv(upstream,buffer.data(),buffer.size(),0))>0)if(!send_all(client,buffer.data(),static_cast<size_t>(count)))break;
        close(upstream);served_region=name;if(name!=region)total_failovers++;return upstream_status;
    }
    reply(client,503,"No CDN edge is available\n","text/plain; charset=utf-8",request.method!="HEAD",config,{{"X-CDN-Visitor-Region",region},{"X-CDN-Decision-Source",source}});return 503;
}

int serve_router(int fd, const Request& request, const Config& config, const sockaddr_storage& peer, std::string& log_region, std::string& log_source) {
    if(request.method!="GET"&&request.method!="HEAD"){reply(fd,405,"Only GET and HEAD are supported\n","text/plain; charset=utf-8",true,config);return 405;}
    if(request.target=="/health"){log_source="router-health";reply(fd,200,"{\"status\":\"ok\"}\n","application/json",request.method!="HEAD",config,{{"Cache-Control","no-store"}});return 200;}
    if(request.target=="/ready"){
        log_source="router-readiness";std::ostringstream body;body<<"{\"status\":\"";bool any=false;const auto states=health_monitor.snapshot();
        for(const auto& state:states)any=any||state.second.healthy;
        body<<(any?"ready":"unavailable")<<"\",\"edges\":{";size_t index=0;for(const auto& state:states){if(index++)body<<',';body<<'"'<<json_escape(state.first)<<"\":"<<(state.second.healthy?"true":"false");}body<<"}}\n";
        const int status=any?200:503;reply(fd,status,body.str(),"application/json",request.method!="HEAD",config,{{"Cache-Control","no-store"}});return status;
    }
    const std::string direct_ip=peer_ip(peer);std::string visitor_ip=direct_ip,source="default";
    if(ipv4_in_cidr(direct_ip,config.trusted_proxy)){const std::string forwarded=header(request,"x-forwarded-for");if(!forwarded.empty())visitor_ip=trim(forwarded.substr(0,forwarded.find(',')));}
    std::string region=config.default_region;const std::string geoip_region=geoip_region_for(visitor_ip);if(geoip_region!="unknown"){region=geoip_region;source="geoip";}
    const std::string state=header(request,"x-india-state");if(!state.empty()&&ipv4_in_cidr(direct_ip,config.trusted_proxy)){const auto mapped=india_region_for(state);if(mapped!="unknown"){region=mapped;source="trusted-state";}}
    const std::string override_region=header(request,"x-cdn-test-region");
    if(config.development_override&&ipv4_in_cidr(direct_ip,"127.0.0.0/8")&&config.edges.count(override_region)){region=override_region;source="dev-override";}
    log_region=region;log_source=source;
    if(request.target=="/metrics"){
        if(!config.metrics_token.empty()&&header(request,"x-cdn-metrics-token")!=config.metrics_token){reply(fd,403,"Metrics access denied\n","text/plain; charset=utf-8",request.method!="HEAD",config);return 403;}
        std::ostringstream body;
        body << "# TYPE cdn_requests_total counter\ncdn_requests_total " << total_requests.load() << '\n'
             << "# TYPE cdn_errors_total counter\ncdn_errors_total " << total_errors.load() << '\n'
             << "# TYPE cdn_failovers_total counter\ncdn_failovers_total " << total_failovers.load() << '\n'
             << "# TYPE cdn_active_connections gauge\ncdn_active_connections " << active_connections.load() << '\n';
        body<<"# TYPE cdn_edge_healthy gauge\n";for(const auto& state:health_monitor.snapshot())body<<"cdn_edge_healthy{region=\""<<json_escape(state.first)<<"\"} "<<(state.second.healthy?1:0)<<'\n';
        reply(fd,200,body.str(),"text/plain; version=0.0.4; charset=utf-8",request.method!="HEAD",config,{{"Cache-Control","no-store"}});return 200;
    }
    if(request.target.rfind("/__cdn/route",0)==0){
        const std::string requested=query_value(request.target,"region");if(config.development_override&&ipv4_in_cidr(direct_ip,"127.0.0.0/8")&&config.edges.count(requested)){region=requested;source="dev-override";}log_region=region;log_source=source;
        bool healthy=health_monitor.healthy(region);std::string selected;
        std::vector<std::string> order{region};for(const auto& edge:config.edges)if(edge.first!=region)order.push_back(edge.first);
        for(const auto& candidate:order){if(health_monitor.healthy(candidate)){selected=candidate;break;}}
        std::ostringstream body;body<<"{\"requested_region\":\""<<json_escape(region)<<"\",\"selected_region\":\""<<json_escape(selected)<<"\",\"decision_source\":\""<<source<<"\",\"visitor_ip\":\""<<json_escape(visitor_ip)<<"\",\"edge_healthy\":"<<(healthy?"true":"false")<<",\"available\":"<<(!selected.empty()?"true":"false")<<"}\n";
        reply(fd,200,body.str(),"application/json",request.method!="HEAD",config,{{"Cache-Control","no-store"},{"X-CDN-Visitor-Region",region},{"X-CDN-Decision-Source",source}});return 200;
    }
    return proxy_request(fd,request,config,region,visitor_ip,source,log_region);
}

void serve_client(int fd, Config config, sockaddr_storage peer) {
    total_requests++;const auto started=Clock::now();set_timeout(fd,config.timeout_ms);int parse_status=0,status=0;std::string path="-",region=config.region,source=config.mode;
    const auto request=read_request(fd,parse_status);
    if(!request){reply(fd,parse_status,status_text(parse_status)+"\n","text/plain; charset=utf-8",true,config);status=parse_status;}
    else{path=request->target;if(config.mode=="router")status=serve_router(fd,*request,config,peer,region,source);else status=serve_edge(fd,*request,config);}
    if(status>=400)total_errors++;close(fd);active_connections--;const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count();log_event(path,region,source,status,elapsed);
}

void worker_loop(const Config& config) {
    while (true) {
        WorkItem item{};
        {
            std::unique_lock<std::mutex> lock(work_mutex);
            work_available.wait(lock, [] { return stopping || !work_queue.empty(); });
            if (work_queue.empty() && stopping) return;
            item = work_queue.front();
            work_queue.pop();
        }
        serve_client(item.fd, config, item.peer);
    }
}

int main(int argc,char* argv[]) {
    Config config;std::string error;
    if(argc>1&&std::string(argv[1])=="router"){
        if(argc!=3||!load_config(argv[2],config,error)){std::cerr<<(error.empty()?"Usage: simple-cdn router <config.toml>":error)<<'\n';return 1;}
    }else{
        if(argc>1)config.root=argv[1];if(argc>2&&!parse_int(argv[2],1,65535,config.port)){std::cerr<<"Invalid port\n";return 1;}
        if(argc>3&&!parse_int(argv[3],0,31536000,config.cache_seconds)){std::cerr<<"Invalid cache duration\n";return 1;}if(argc>4)config.region=argv[4];
        try{config.root=fs::canonical(config.root);if(!fs::is_directory(config.root))throw fs::filesystem_error("not directory",config.root,std::make_error_code(std::errc::not_a_directory));}catch(const fs::filesystem_error&){std::cerr<<"Asset directory does not exist: "<<config.root<<'\n';return 1;}
    }
    if(const char* token=std::getenv("CDN_ROUTER_TOKEN"))config.router_token=token;
    if(const char* token=std::getenv("CDN_METRICS_TOKEN"))config.metrics_token=token;
    if(!config.geoip_database.empty()&&!initialize_geoip(config.geoip_database))std::cerr<<"Warning: GeoIP database unavailable; using fallback routing\n";
    std::signal(SIGPIPE,SIG_IGN);std::signal(SIGINT,request_shutdown);std::signal(SIGTERM,request_shutdown);const int server=socket(AF_INET,SOCK_STREAM,0);if(server<0){std::perror("socket");return 1;}listening_socket=server;int enabled=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(static_cast<uint16_t>(config.port));if(inet_pton(AF_INET,config.listen.c_str(),&address.sin_addr)!=1){std::cerr<<"Invalid listen address\n";return 1;}
    if(bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0||listen(server,128)<0){std::perror("bind/listen");close(server);return 1;}
    if(config.mode=="router")health_monitor.start(config);
    std::vector<std::thread> workers;workers.reserve(static_cast<size_t>(config.worker_threads));for(int i=0;i<config.worker_threads;++i)workers.emplace_back(worker_loop,std::cref(config));
    std::cout<<"{\"event\":\"started\",\"mode\":\""<<config.mode<<"\",\"port\":"<<config.port<<",\"workers\":"<<config.worker_threads<<"}\n"<<std::flush;
    while(!stopping){sockaddr_storage peer{};socklen_t length=sizeof(peer);const int client=accept(server,reinterpret_cast<sockaddr*>(&peer),&length);if(client<0){if(stopping)break;if(errno==EINTR)continue;std::perror("accept");continue;}if(active_connections.fetch_add(1)>=config.max_connections){active_connections--;reply(client,503,"Server is busy\n","text/plain; charset=utf-8",true,config);close(client);continue;}{std::lock_guard<std::mutex> lock(work_mutex);work_queue.push({client,peer});}work_available.notify_one();}
    work_available.notify_all();for(auto& worker:workers)worker.join();if(config.mode=="router")health_monitor.stop();std::cout<<"{\"event\":\"stopped\",\"mode\":\""<<config.mode<<"\"}\n"<<std::flush;return 0;
}
