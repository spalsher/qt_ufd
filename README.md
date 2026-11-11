# MEW UFD V11 — plate / fare / status

ESP32 + W5500 Ethernet + three 64×32 RGB HUB75 panels, stacked 64×96.
Based on the supplied `TOLL_DISPLAY_V10_OLD_UFD` source. This is an updated
source package, not a compiled or flashed firmware release.

## What changed

- Top panel: white LPR plate with red Ajrak border. No “SINDH” text.
- Middle panel: fare, or paid amount. Bottom: PLEASE PAY, PLEASE WAIT,
  THANK YOU, RFID remaining balance, or assistance.
- Registration remains visible across payment states. Nothing scrolls during a transaction.
- Waiting/checking/assistance no longer disappear after the old 10-second timer.
  They have a 120-second default lease. If updates stop, show EXPIRED / SEE STAFF,
  hiding the old fare rather than showing an indefinite stale charge.
- Paid and RFID-paid summaries remain for six seconds, then idle. A newer
  confirmed vehicle update can replace them immediately. No blocking sleep.
- Closed stays until an explicit `open` update. Display-only: this never opens a barrier.
- Authenticated structured API; boot session + increasing sequence + short command
  deadline. Duplicate requests acknowledge without redrawing or extending the timer.
- Invalid/unknown commands return errors. An ACK follows framebuffer submission;
  it does not prove that LEDs physically lit or a driver saw the message.
- Legacy URLs remain as an opt-in migration aid. Old Welcome requests cannot
  replace active transaction screens. After the first v1 frame, legacy display
  writes are rejected for the remainder of that ESP32 boot.
- Bounded HTTP input (1-second total header deadline, 1KB request line, 2KB headers),
  numeric/plate validation, default-disabled administrative/test endpoints.
- 30-second command-task watchdog; stopped Ethernet driver retried using the existing
  10-second startup retry. Cable unplug alone does not trigger a reboot.
- Image reads capped at 64KB/20 seconds; replacement no longer deletes the working
  image before rename. Mount failure no longer auto-formats the asset filesystem.
  Built-in pictures work without a mounted filesystem or picture server.

## Build and configure

Keep the original sketch folder name: open `TOLL_DISPLAY/TOLL_DISPLAY.ino`.
The code targets **Arduino ESP32 core 3.x** (`NetworkClient`, `NetworkServer`,
SPI Ethernet and the ESP-IDF 5 watchdog API). Use the same proven 3.x core and
library versions as the existing booth build. The uploaded source did not supply
version pins, so a reproducible hardware build remains to be established.

Libraries:
- Adafruit GFX Library + Adafruit BusIO
- ESP32 HUB75 LED MATRIX PANEL DMA Display (mrcodetastic)

Board: ESP32 Dev Module. Existing README recommended “Default 4MB with spiffs”.
Keep the deployed flash/partition settings; changing them can lose saved settings/assets.
W5500 and HUB75 pins, scan mapping, color depth and DMA timing are unchanged.
Existing NVS network and panel settings are retained. Source default IP is
`192.168.78.12` (the old README incorrectly said `.30`). Check the boot log for the actual IP.

Edit `TOLL_DISPLAY/UfdConfig.h` before compiling:

```cpp
#define UFD_API_TOKEN "YOUR_UNIQUE_RANDOM_TOKEN"
#define UFD_ALLOW_LEGACY 0
#define UFD_ENABLE_ADMIN 0
#define UFD_ACTIVE_LEASE_MS 120000UL
```

Generate a token on the developer's machine:

```sh
python3 -c 'import secrets; print(secrets.token_hex(32))'
```

Keep the token out of Git and URLs. Provision the same token to the single booth
sender. Empty token with legacy disabled makes requests fail with
`503 configure_api_token`; there is no shipped universal password.
HTTP is unencrypted: isolate the UFD network and allow connections only from the
booth controller. A bearer token is not a substitute for network isolation.
Administrative endpoints require both `UFD_ENABLE_ADMIN=1` and a nonempty token;
all calls then need the Authorization header, including the configuration page.

No automatic filesystem format is performed. On a new board, built-in pictures
are available immediately. Provision/mount LittleFS separately if downloaded
pictures are required; do not erase the existing production filesystem to test.
The original restricted, uncompressed RGB/RGBA PNG format remains required.
This release does not add general PNG/GIF/video decoding.

## QuickToll and MewRF migration

The shared display controller must be the **only v1 sender**. It combines the
current passage's LPR, fare, payment state and RFID balance, then sends a complete
frame. It must not forward every nearby RFID read or unconfirmed LPR change.
Use `API.md`. This package does not change either application's source or install
a booth controller. Until those callers are migrated, the firmware alone cannot
identify whether two different legacy plates belong to one vehicle or two.

For an isolated legacy bench test only, set `UFD_ALLOW_LEGACY=1`. Existing clients
without Authorization work only with an empty token. This is not the production
security configuration. With a token, old clients must add the header.
Supported compatibility commands:

- `/?vehicle_number=AAA000&fare_amount=200` → new plate/fare/pay layout
- `/?vehicle_number=AAA000&metag_paid=100&balance=900` → plate/fare/balance
- `/?vehicle_number=AAA000&metag_low` → plate / payment required / see staff
- `/thankyou` → same plate/fare with THANK YOU; requires an active fare
- `/?vehicle_number=WELCOME&take_slip` → idle only if no active transaction
- `/?vehicle_number=BOOTH&CLOSED`, `/closed`, `/open`
- `/takeslip`, `/?vehicle_number=AAA000&take_slip` retain legacy ticket graphics
- bare `/` and `/status` provide status; unknown commands fail

Legacy requests cannot provide full ordering, expiry or vehicle ownership. They
must not remain active alongside v1. No automatic fallback from v1 to legacy.

## Verification performed here

```sh
g++ -std=c++11 -Wall -Wextra -Werror tests/policy_test.cpp -o /tmp/ufd-policy-test
/tmp/ufd-policy-test
python3 tests/run_api_tests.py
python3 tests/run_render_tests.py
```

Host tests exercise the actual policy, API/validation functions and drawing
functions, with simulated Arduino I/O. They cover stale and conflicting updates,
duplicate replay, boot mismatch, expiry, closure protection, lease/summary timing,
`millis()` rollover, field limits and pixel bounds. Rendered PPM files in `preview/`
come from the actual firmware drawing code, not the earlier concept mockup.

**Not performed:** ESP32 cross-compilation, flashing, DMA/electrical/brightness
checks, physical network recovery, power-cut testing, or a 24-hour soak test.
No Arduino CLI is installed in the review environment; its download returned
HTTP 403. Do not treat host tests as a successful ESP32 build.

## Bench acceptance before booth rollout

1. Compile using the booth's toolchain; record exact versions and firmware hash.
2. Flash a spare controller; verify panel order, brightness, readability and `ready:true`.
3. Run `tools/send_frame.py` sequentially (API examples below), including a long plate
   and largest real fare/balance. Test decimals if used at the plaza.
4. Verify repeated same-frame retries do not replay or extend screens; delayed older
   frames return 409; latest higher-sequence confirmed vehicle takes over promptly.
5. Unplug Ethernet during payment. Confirm EXPIRED / SEE STAFF after the lease,
   reconnect, and send only a freshly confirmed current frame. Do not replay a backlog.
6. Restart ESP32: old bootId packets must fail; obtain a new session and current data.
7. Test Closed, attempted payment while Closed, and explicit Open.
8. Test picture-server outage, filesystem unavailable, and power interruption during
   asset replacement; built-in images must remain usable.
9. Soak with real maximum request rates for 24 hours, observing heap, resets and flicker.

Keep the existing flashed binary and NVS/partition settings for rollback.
Payment and Gatekeeper must continue independently of UFD availability.
