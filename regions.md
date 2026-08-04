# India edge regions

| Region | Edge city | States and territories |
| --- | --- | --- |
| `north` | Delhi NCR | Chandigarh, Delhi, Haryana, Himachal Pradesh, Jammu & Kashmir, Ladakh, Punjab, Rajasthan, Uttar Pradesh, Uttarakhand |
| `west-central` | Mumbai | Chhattisgarh, Dadra & Nagar Haveli and Daman & Diu, Goa, Gujarat, Madhya Pradesh, Maharashtra |
| `south-east` | Bengaluru | Andhra Pradesh, Arunachal Pradesh, Assam, Bihar, Jharkhand, Karnataka, Kerala, Manipur, Meghalaya, Mizoram, Nagaland, Odisha, Puducherry, Sikkim, Tamil Nadu, Telangana, Tripura, West Bengal, Andaman & Nicobar, Lakshadweep |

The C++ router implements this mapping for MaxMind subdivision ISO codes and trusted `X-India-State` values. Unknown, non-India, or unavailable GeoIP results use the configured default region. Edge reachability failure triggers automatic failover to another configured edge.

See [README.md](README.md) for local startup and manual verification.
