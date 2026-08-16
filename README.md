# HPA300 Firmware

Custom ESP32-S2 firmware for an HPA300 HEPA filter controller. The controller
provides a capacitive touch interface, PWM-dimmable status LEDs, and mutually
exclusive fan selection through a 74HC238 decoder.

## Supported environment

- ESP-IDF v5.5.1
- ESP32-S2 target

The target is recorded in `sdkconfig.defaults`, and the development container is
pinned to ESP-IDF v5.5.1. A normal command-line build is:

```text
idf.py set-target esp32s2
idf.py build
```

The application uses size optimization and a dual-slot OTA partition table.
Builds require a local RSA-3072 signing key at
`secrets/hpa300-ota-signing-key.pem`; the directory is ignored by Git. Generate
the key once from an ESP-IDF terminal and keep encrypted off-machine backups:

```text
espsecure.py generate_signing_key --version 2 --scheme rsa3072 secrets/hpa300-ota-signing-key.pem
```

An ordinary build produces `build/HPA300-FIRMWARE-unsigned.bin` and the signed
`build/HPA300-FIRMWARE.bin`. Only the signed file may be flashed or uploaded.
The build fails if the signed image leaves less than 64 KiB in an OTA slot. A
release should be made from a clean, tagged commit and verified before use:

```text
espsecure.py verify_signature --version 2 --keyfile secrets/hpa300-ota-signing-key.pem build/HPA300-FIRMWARE.bin
```

On Windows, run the check from an activated ESP-IDF terminal and use
`espsecure.exe` in place of `espsecure.py`.

A clean commit with an exact `v*` tag embeds that tag as the application
version. Untagged or dirty development builds embed the commit plus a UTC
configure timestamp, for example `168c0e9-dev-20260815T153000Z`, so consecutive
bench images are distinguishable. Run `idf.py fullclean build` when deliberately
producing a new development image. CI or a release candidate can override the
value before configuration with `$env:HPA300_VERSION = "v1.0.0-rc1"` in
PowerShell. Versions must fit ESP-IDF's 31-character application-version field.

The ESP32-S2 application console uses the board's native USB-C connection via
USB CDC. For an initial flash, hold Boot, tap Reset, then release Boot to enter
ROM download mode. After flashing, the application console may enumerate under
a different COM port; list ports again before starting `idf.py monitor`.

The board mapping/invariant test application can be built separately:

```text
idf.py -C test -B build-test build
```

## Wi-Fi setup and local API

Networking is optional and starts only after fan control, LEDs, the controller,
and touch input are operational. A network initialization failure does not stop
local appliance control.

On first boot, join the open access point named `HPA300-xxxxxx`. Most clients
will open the setup page automatically; otherwise browse to `http://192.168.4.1/`.
Enter the Wi-Fi SSID, Wi-Fi password, and a private 16-64 character API token.
The controller stores the settings in NVS, restarts, joins the configured
network, and closes the setup access point.

The UART log reports provisioning AP activation, reconnect attempts, the
station IP address, and HTTP API startup. During bring-up, the front-panel
LED2 (the Check Filters indicator) also shows network status:

| LED2 pattern | Network status |
| --- | --- |
| Fast blink | Provisioning portal active |
| Slow blink | Connecting or waiting to retry |
| Solid | Wi-Fi connected and HTTP API ready |
| Very fast blink | Firmware upload in progress |
| Off | Network subsystem unavailable |

LED2 uses the panel's shared brightness setting, so it is intentionally dark
when the user-selected LED brightness is Off.

After enrollment, tapping keys `4, 5, 4, 5` within ten seconds opens a
ten-minute physical maintenance window and enables the setup AP in AP+STA mode.
A Wi-Fi outage by itself never exposes the unauthenticated setup portal or
permits a firmware write. Reserve the controller's address in the router's DHCP
configuration because this phase intentionally does not use mDNS or Zeroconf.

The version 1 HTTP API listens on port 80:

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/api/v1/device` | Identity, reset/runtime diagnostics, control health, and RAM-only flash counters |
| `GET` | `/api/v1/state` | Applied/desired fan state, command sequences, timer, fault, and LED state |
| `PUT` | `/api/v1/fan` | Queue `{"speed": 0}` through `{"speed": 4}` and return `202 Accepted` |
| `GET` | `/api/v1/ota` | Running slot/version, maintenance state, and previous firmware |
| `POST` | `/api/v1/ota` | Stream a signed application image into the inactive OTA slot |
| `POST` | `/api/v1/ota/rollback` | Validate and boot the previous firmware slot |

Operational API requests require `Authorization: Bearer <token>`; the only
exception is signed OTA recovery through the AP during a physical maintenance
window. The token is protected from unauthenticated API clients, but HTTP does
not encrypt it on the LAN. Use this phase on a trusted network. A REST fan package example is in
`home-assistant/hpa300.yaml.example`; it polls actual controller state every
five seconds and exposes a standard Home Assistant fan with four 25% steps.
Copy it into a Home Assistant packages directory, add the documented URLs and
authorization value to `secrets.yaml`, and reserve the purifier's DHCP address.

The package also defines a read-only `sensor.hpa300_diagnostics` heartbeat. It
polls `/api/v1/device` every 30 seconds and records every response, including
the firmware version, `last_boot` reset details, and runtime uptime/free-heap
watermarks, actor timing/counters, latest fault, and boot-scoped flash activity,
so overnight availability and reboot causes can be reviewed in
Home Assistant History. Remove
`force_update: true` after active troubleshooting if the per-poll history is no
longer useful.

Crash dumps use the compact binary format and are stored in the dedicated
64 KiB coredump partition. The first crash is retained across a later reset
cascade. Keep the matching `build/HPA300-FIRMWARE.elf`; with the affected unit
in ROM download mode, inspect the retained dump before reflashing:

```text
idf.py -p COMx coredump-info
```

`GET /api/v1/device` and `GET /api/v1/ota` include a `last_boot` object with
the ESP-IDF reset name/code, whether it is power-related, and whether a software
reset was deliberately scheduled for OTA, rollback, or provisioning. The same
information appears on `/update`. Reset causes and planned-reset markers use
reset/RTC memory only; collecting these diagnostics never writes flash during a
power failure.

Commands received through the REST API cancel any active local timed-off mode,
so a manual timer cannot unexpectedly defeat Home Assistant control. The
length-one command mailbox intentionally coalesces bursts: a `202` response
means the command was accepted and assigned a sequence, while `/api/v1/state`
reports whether that sequence was applied or superseded.

## Firmware updates

Before making USB or service-pad access difficult, complete the
[pre-assembly test plan](docs/preassembly-test-plan.md).

Open `http://<controller-address>/update`, enter the API token, perform the
physical `4, 5, 4, 5` gesture, and select the signed
`build/HPA300-FIRMWARE.bin`. The same page is available through the temporary
`HPA300-xxxxxx` AP at `http://192.168.4.1/update`. LAN uploads require the API
token. AP uploads require the physical maintenance window and a firmware image
signed by the controller's current signing key, so recovery remains possible
if the stored token is unavailable.

OTA startup erases the entire inactive application partition before streaming,
including the Secure Boot v2 signature sector. This prevents a shorter or
unsigned upload from inheriting a valid signature left by an older image.

The equivalent LAN command is:

```powershell
$env:HPA300_TOKEN = "your-private-token"
curl.exe --fail-with-body `
  -H "Authorization: Bearer $env:HPA300_TOKEN" `
  -H "Content-Type: application/octet-stream" `
  --data-binary "@build/HPA300-FIRMWARE.bin" `
  "http://CONTROLLER-IP/api/v1/ota"
```

An upload stops the fan, rejects the wrong chip/project and images larger than
the inactive slot, and streams directly to flash. ESP-IDF validates the full
image and RSA signature before changing the boot partition. The new image then
has a 30-second probation period; failure to initialize the controller and HTTP
server, or a reset before confirmation, returns to the previous slot. Router or
Internet connectivity is not required to pass probation. The update page also
allows a physically authorized return to the previous valid slot.

## First OTA-capable installation and UART recovery

The currently shipped single-app partition table must be replaced once over a
wired ROM-download connection. Before erasing anything, hold Boot, tap Reset,
release Boot, and confirm the physical flash size:

```text
esptool.py --chip esp32s2 -p COMx flash_id
```

This controller reports 4 MB, so the project defaults to `partitions-4mb.csv`
with two 1.875 MiB OTA slots. `partitions.csv` remains available for a verified
2 MB board, but must never be replaced by the 4 MB layout unless `flash_id`
reports sufficient capacity. Then perform the one-time migration:

```text
idf.py fullclean build
espsecure.py verify_signature --version 2 --keyfile secrets/hpa300-ota-signing-key.pem build/HPA300-FIRMWARE.bin
idf.py -p COMx erase-flash
idf.py -p COMx flash monitor
```

Erasing is intentional because the new OTA metadata overlaps the old NVS
layout; Wi-Fi and the API token must be provisioned again. Confirm the boot log
reports `ota_0` before closing the enclosure.

Hardware Secure Boot, flash encryption, anti-rollback eFuses, and ROM download
restrictions remain disabled. If OTA and networking are unusable, reopen the
enclosure, reach Boot/Reset plus native USB or the TC2030 UART pads, enter ROM
download mode, and repeat `idf.py erase-flash` followed by `idf.py flash`. This
can replace the bootloader, partition table, firmware, and signing key. Keep the
service pads accessible and record their enclosure location before packaging.
Complete the hardware acceptance checklist in `docs/ota-validation.md` before
making the wired service connection difficult to reach.

## Safety model

The 74HC238 provides the hardware single-active-output invariant. Firmware
disables the decoder before changing its address and initializes a statically
allocated, high-priority fan actor before touch, LEDs, or networking. Only that
actor calls the HC238 driver. Duplicate speed requests perform no GPIO writes,
and obsolete pending commands are overwritten rather than replayed. A driver
failure makes one best-effort OFF request and latches fan control across reset;
only a cold power cycle clears the latch, and boot never replays the prior fan
state. OTA and provisioning writes require a bounded OFF acknowledgement first.

The active-high E3 input is held inactive by
R18, a 10 kΩ hardware pull-down, so fan outputs remain disabled before the
ESP32-S2 starts.

## Board mapping

Pin assignments live in `components/board/include/board.h`. The mappings below
were verified against the `HPA300-hardware` KiCad schematic and PCB. KiCad's
schematic-parity check reports no differences between them.

Current touch mapping:

| Logical key | Touch channel / GPIO |
| --- | --- |
| 1 | 3 |
| 2 | 2 |
| 3 | 1 |
| 4 | 5 |
| 5 | 6 |
| 6 | 4 |

Current controls:

| Key | Action |
| --- | --- |
| 1 | Cycles `off → fan 1 → fan 2 → fan 3 → off`. From fan 4, selects off. |
| 2 | Toggles fan 4 and off. From any other active speed, selects fan 4. |
| 3 | Directly cycles unified LED brightness: High (100%), Medium (50%), Low (5%), Off (0%). |
| 6 | While a fan is active, cycles the automatic shutoff: 2 h (LED9), 4 h (LED6), 8 h (LED3), then Always-on (no timer LED). Selecting a timed mode starts its countdown. The current test build uses 2, 4, and 8 seconds respectively. |
| 1, 2, 4, or 5 | If the intended brightness is below High, temporarily sets LEDs to High for three seconds after the last non-dimmer touch, then restores the intended brightness. |

Crossing the fan Off position in either direction resets the intended LED brightness to High. For example, after an extended idle period at Off, key 1 selects fan 1 and leaves the LEDs at High.
Reaching Off also cancels any active fan shutoff timer. Changing between active fan speeds preserves the running timer.

Current HC238 mapping:

| Fan speed | HC238 output |
| --- | --- |
| 1 | Y0 |
| 2 | Y2 |
| 3 | Y4 |
| 4 | Y6 |

HC238 A0, E1, and E2 are tied to ground. A1 is GPIO18, A2 is GPIO17,
and active-high E3 is GPIO16. R16, R17, and R18 provide 10 kΩ pull-downs
on those three MCU-controlled inputs. Decoder outputs Y0, Y2, Y4, and Y6
drive FAN1-FAN4 respectively through channels 1, 3, 5, and 7 of the TBD62783A.

Verified LED mapping:

| LED | GPIO | Function |
| --- | --- | --- |
| LED1 | 12 | Fan speed 3 |
| LED2 | 15 | Check filters; temporary Wi-Fi status overlay |
| LED3 | 8 | 8-hour timer |
| LED4 | 13 | Fan speed 2 |
| LED5 | 7 | Check pre-filter |
| LED6 | 9 | 4-hour timer |
| LED7 | 14 | Fan speed 1 |
| LED8 | 11 | Fan speed 4 |
| LED9 | 10 | 2-hour timer |
