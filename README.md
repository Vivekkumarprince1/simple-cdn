# Simple CDN

A dependency-light C++17 regional CDN prototype. One public router selects a healthy India edge, reverse-proxies the request, and automatically fails over when the preferred edge is unavailable. Edge processes safely stream static assets and can reject requests that do not carry the shared router token.

## Highlights

- Three India edge regions behind one public reverse-proxy router
- Persistent MaxMind GeoIP lookup with trusted-proxy and safe fallback routing
- Active authenticated health monitoring, circuit breaking, recovery, and cross-region failover
- Fixed worker pools, bounded queues, header limits, and network timeouts
- Static-file streaming with `ETag`, conditional `304`, and byte-range `206` responses
- Traversal and symlink protection, router-only edges, and sanitized forwarding headers
- Prometheus-format metrics and structured JSON logs

## Operations console

The browser UI is a live CDN operations console rather than a static mockup. It consumes the router APIs directly and includes:

- Responsive circuit topology for Delhi, Mumbai, and Bengaluru
- Live healthy, degraded, circuit-open, and unavailable states
- City-based and opt-in current-location routing
- Visible preferred-route and failover paths
- Auto-refreshing request, error, failover, connection, and edge-health telemetry
- Per-edge routing actions and a browser-session activity log
- Loading, error, retry, permission-denied, and empty states
- Keyboard focus, semantic landmarks, accessible status announcements, reduced-motion support, and a strict Content Security Policy

Frontend concerns are separated into [public/index.html](public/index.html), [public/styles.css](public/styles.css), and [public/app.js](public/app.js).

## Architecture

```text
visitor -> router :8080 -> north edge        :8081
                        -> west-central edge :8082
                        -> south-east edge   :8083
```

The router chooses a region in this order:

1. GeoLite2/GeoIP subdivision lookup when built with `libmaxminddb` and a database is configured.
2. `X-India-State` from a client inside the configured trusted-proxy CIDR.
3. Loopback-only `X-CDN-Test-Region` or route-console override when development overrides are enabled.
4. Configured default region.

If the selected edge cannot be reached, the router tries the remaining configured edges. It never trusts public `X-Forwarded-For`, state, or test-region headers.

## Build

```bash
./scripts/build.sh
```

`libmaxminddb` is detected through `pkg-config`. Without it, the same binary remains functional and safely uses trusted-state or default routing.

The implementation is separated by responsibility:

```text
src/cdn_types.hpp  shared runtime models
src/config.cpp     validated TOML-style configuration
src/geoip.cpp      process-lifetime MaxMind database resolver
src/main.cpp       HTTP, edge serving, routing, proxying, workers and metrics
```

## Run locally

From the project directory, start the complete stack with one command:

```bash
cd /Users/vivekkumar/devlopment/simple-cdn
./scripts/run-local.sh
```

Wait for this message:

```text
Simple CDN is running at http://localhost:8080
Press Ctrl+C to stop all four services.
```

Keep the terminal open and visit [http://localhost:8080](http://localhost:8080). Press `Ctrl+C` in the terminal to stop the router and all three edges. The process interruption is normal.

### Use the browser demo

- Select a city and click **Route request** to exercise its regional edge.
- Click **Use current location** to grant optional browser location permission and choose the closest edge.
- Exact coordinates are not displayed or sent to the C++ server. The browser calculates only the nearest requested region; the router still performs health checking and failover.
- If the page looks stale, use a hard refresh (`Cmd+Shift+R` on macOS).

The map displays all three active regions:

| Region | Edge | Local port |
| --- | --- | --- |
| `north` | Delhi | `8081` |
| `west-central` | Mumbai | `8082` |
| `south-east` | Bengaluru | `8083` |

### View logs

Requests are printed as structured JSON in the terminal running `./scripts/run-local.sh`:

```json
{"path":"/","region":"north","source":"edge","status":200,"duration_ms":0}
```

Save and view the complete session with:

```bash
./scripts/run-local.sh 2>&1 | tee cdn.log
```

From another terminal, follow it with:

```bash
tail -f cdn.log
```

### Verify every edge manually

With `./scripts/run-local.sh` still active, send one request to each region:

```bash
curl -I -H 'X-CDN-Test-Region: north' http://localhost:8080/
curl -I -H 'X-CDN-Test-Region: west-central' http://localhost:8080/
curl -I -H 'X-CDN-Test-Region: south-east' http://localhost:8080/
```

Inspect `X-CDN-Edge-Region` in each response. Check routing health directly with:

```bash
curl 'http://localhost:8080/__cdn/route?region=north'
curl 'http://localhost:8080/__cdn/route?region=west-central'
curl 'http://localhost:8080/__cdn/route?region=south-east'
```

View runtime metrics:

```bash
curl http://localhost:8080/metrics
```

Metrics include total requests, errors, failovers, active connections, and per-region edge health.

Check process liveness and edge readiness separately:

```bash
curl http://localhost:8080/health
curl http://localhost:8080/ready
```

### Demonstrate failover

For a manual failover demonstration, start the components separately using the commands below. Stop one edge with `Ctrl+C`, request that region through port `8080`, and observe `X-CDN-Edge-Region` identify a different healthy edge.

### Manual startup

To start each component manually, use four terminals as described below.

Start the three protected edges:

```bash
CDN_ROUTER_TOKEN=local-development-token ./simple-cdn public 8081 86400 north
CDN_ROUTER_TOKEN=local-development-token ./simple-cdn public 8082 86400 west-central
CDN_ROUTER_TOKEN=local-development-token ./simple-cdn public 8083 86400 south-east
```

Then start the router:

```bash
./simple-cdn router config/cdn.local.toml
```

Open `http://localhost:8080`. The page queries the real router and displays health/failover results. Verify from the command line with:

```bash
curl -i -H 'X-CDN-Test-Region: west-central' http://127.0.0.1:8080/
curl -s 'http://127.0.0.1:8080/__cdn/route?region=south-east'
```

## Troubleshooting

### `localhost refused to connect`

Nothing is listening on port `8080`. Start the complete stack and keep its terminal open:

```bash
./scripts/run-local.sh
```

Confirm the router is reachable:

```bash
curl -I http://localhost:8080
```

### Current location does not work

- Open the page through `http://localhost:8080`, not as a local `file://` document.
- Allow location access when the browser asks.
- If access was previously denied, reset the location permission for `localhost` in browser site settings.
- The city selector remains available when browser geolocation is unsupported or denied.

### Port already in use

Find the process holding a CDN port:

```bash
lsof -nP -iTCP:8080 -sTCP:LISTEN
```

Stop an earlier `./scripts/run-local.sh` terminal before starting another stack.

## Suggested project demonstration

1. Run `./scripts/run-local.sh` and open the browser console.
3. Route Delhi, Mumbai, and Bengaluru requests and show the response region changing.
4. Use **Use current location** to demonstrate opt-in nearest-edge selection.
5. Stop one manually started edge and demonstrate automatic failover.
6. Show traversal protection and direct-edge rejection manually.

The key explanation is: one public URL reaches the router, the router derives a preferred India region, verifies edge availability, and streams the selected edge response. If that edge is unavailable, another configured edge preserves availability.

## Edge mode

```text
simple-cdn [asset-directory] [port] [cache-seconds] [region]
```

It supports `GET`, `HEAD`, `/health`, MIME types, CORS, CDN headers, `ETag` validation, byte ranges, bounded headers and worker-pool concurrency, socket timeouts, traversal/symlink containment, and chunked file streaming. Set `CDN_ROUTER_TOKEN` to require router authentication.

Every request is logged as one JSON line containing path, region/mode, source, status, and duration.
# simple-cdn
