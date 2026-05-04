# Security

This project tunnels application traffic via TON ADNL and local SOCKS bridging. Treat it like **privileged network infrastructure**:

- Rotate `auth_token` and private keys on compromise.
- Prefer **minimal** egress policies (blocked private ranges are implemented in server code — review `PROJECT_OVERVIEW_RU.md`).
- Firewall the listen UDP/TCP ports and monitor logs for abuse.

Report suspected vulnerabilities privately to the maintainer via GitHub Security Advisories (**Security** tab → **Report a vulnerability**) or the contact linked from the repository owner profile. Please include reproducible steps and affected revision.

Do not disclose exploitable bugs in public issues before a coordinated fix window.

Russian companion: [`SECURITY.ru.md`](SECURITY.ru.md).
