# CHANGELOG


## v1.3.0-beta.1 (2026-08-03)

### Bug Fixes

- **tests**: Pin the Kivy window provider so headless runs can't exit(102)
  ([`fd5e359`](https://github.com/bartei/drdro-mainboard/commit/fd5e359b433da76ac85da117ceddb181c7004c8f))

- **tests**: Wait for accept() before dropping the mock board connection
  ([`b08640a`](https://github.com/bartei/drdro-mainboard/commit/b08640a26a6422275232b5209a5d4593da788bc6))

### Chores

- **hardware**: Update board project file; remove H723 variant design notes
  ([`4ed6759`](https://github.com/bartei/drdro-mainboard/commit/4ed6759ff717ab47c1a4e2f521bbf1c3715ec5d3))

- **software**: Add drDRO host software — Kivy port with Ethernet transport
  ([`e539b67`](https://github.com/bartei/drdro-mainboard/commit/e539b67091efeb1d515c2a923027dc1f214adbb4))

### Continuous Integration

- **release**: Surface host-test failures as annotations
  ([`8830f8b`](https://github.com/bartei/drdro-mainboard/commit/8830f8b967db2692c5010840cca3e57955a71d82))

### Features

- **update**: Single-version stack releases with one-step updates
  ([`6e24594`](https://github.com/bartei/drdro-mainboard/commit/6e24594c8ff1fcbf513d44d6a5ecc492fefd4e0f))


## v1.2.0 (2026-08-01)

### Features

- **protocol**: Configurable RS-485 baud up to 1 Mbaud (com.baud)
  ([`a6545db`](https://github.com/bartei/drdro-mainboard/commit/a6545dbf303b7801559bfebf5b0e0ef854b683f7))


## v1.1.1 (2026-08-01)

### Bug Fixes

- **boot**: Harden the bootloader-jump path found by RS-485 update testing
  ([`bcfb676`](https://github.com/bartei/drdro-mainboard/commit/bcfb676559fb0302801f65e07faffac5d21c8b49))


## v1.1.0 (2026-08-01)

### Bug Fixes

- **boot**: Survive a hung I2C bus at startup
  ([`73e19b1`](https://github.com/bartei/drdro-mainboard/commit/73e19b1443aecc818b0317f092e7e6ff6a3b056a))

### Features

- **protocol**: Report encoder count as read-only scales.count
  ([`0dc3c82`](https://github.com/bartei/drdro-mainboard/commit/0dc3c824603b56a8e88b144aa905cd6c10366872))


## v1.0.1 (2026-08-01)

### Bug Fixes

- **release**: Regenerate CHANGELOG.md on every release
  ([`ca3e73d`](https://github.com/bartei/drdro-mainboard/commit/ca3e73d9b84efee794b91dc2e629c2db601065ca))


## v1.0.0 (2026-08-01)

- Initial Release
