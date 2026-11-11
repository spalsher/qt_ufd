# UFD V11 API

All endpoints require `Authorization: Bearer <UFD_API_TOKEN>` in the production
configuration. Use one serialized sender per booth. ESP32 handles one frame at a
time; it does not hold a backlog of display events.

## GET /v1/status

Returns `bootId`, last accepted `sequence`, `passageId`, actual current `mode`,
`ready`, `controlled`, `authenticated`, `uptimeMs`, and `freeHeap`.
`ready` means panel driver initialization succeeded, not physical panel feedback.

At startup/reconnect, read status. Pick the next sequence greater than the returned
value. Do not let QuickToll and MewRF each independently allocate sequence numbers.
The shared controller owns that counter and current passage.

## POST /v1/frame

Parameters are in the URL query; body is empty. This avoids adding a JSON parsing
library to the ESP32. URL-encode all values; never put the secret in the URL.
The response is JSON. Do not send a JSON body: required query fields will be missing.

| Parameter | Meaning |
|---|---|
| `bootId` | Exact value returned by the current ESP32 boot |
| `sequence` | Increasing integer 1–4294967295, global for this ESP32 boot |
| `expiresAtMs` | Command deadline in ESP32 uptime milliseconds, modulo 2^32; must be future and at most 10 seconds away |
| `passageId` | Required for transaction states; 1–48 letters, digits, hyphen or underscore |
| `state` | `waiting`, `checking`, `paid`, `rfid-paid`, `assist`, `idle`, `closed`, `open` |
| `plate` | LPR result for this passage; up to 12 supported characters and must fit the smallest font; omit if unknown |
| `fare` | Nonnegative amount, at most 8 characters and two decimal places; required for waiting/paid/rfid-paid |
| `balance` | Same amount format; required for rfid-paid |
| `leaseMs` | Optional active-screen lease, 5000–300000; defaults to 120000 |

Use `expiresAtMs = (status.uptimeMs + 5000) mod 2^32` immediately after reading
status. A continuously connected controller can use a monotonic offset from that
sample; resynchronize after disconnection. Do not compute this from wall-clock time.
If read/send took too long, obtain fresh status and reconsider the current vehicle;
never refresh the deadline on an old event just to force it through.

For retries, send the **identical parameters** with the same sequence, including
the original deadline. The exact last accepted frame gets `200 duplicate` without
redraw or lease extension, even if its screen has since expired. Older sequences
return 409. Changing content or deadline under the same sequence returns 409.
No completed financial approval is persisted/replayed across ESP32 reboot.

Responses:
- `200`, `result: applied`: valid frame submitted to the display driver
- `200`, `result: duplicate`: previously accepted; not redrawn
- `400`: invalid fields, missing information, unknown state or parameter
- `401`: invalid/missing token
- `403`: administration disabled
- `404`: unsupported endpoint/method
- `409`: old sequence, conflicting reuse, old boot, expired deadline, closed lane,
  or legacy command after v1 takes control
- `503`: missing token configuration or panel driver unavailable

A new, valid higher-sequence frame may replace a six-second summary for the next
confirmed vehicle. The controller is responsible for passage ownership and for
preventing old RFID/LPR information from becoming a new frame.

`closed` locks the display. Only `open` unlocks it, moving to idle; this is a DISPLAY
operation, not a Gatekeeper command. `idle` cannot unlock Closed. A reboot starts in
idle; the controller must restore current lane status. Do not rely on this display
as the authoritative lane closure interlock.

A waiting/checking/assist frame expires to EXPIRED / SEE STAFF if its lease is not
renewed. Renew only a still-current passage with a new sequence, typically every
30 seconds. Paid/rfid-paid expire to idle after six seconds regardless of leaseMs.

## Sequential bench example

`tools/send_frame.py` obtains status, builds a fresh frame and retries a lost reply
once using the same parameters. It is a bench tool, not a multi-client controller.

```sh
export UFD_URL=http://192.168.78.12
read -rs -p 'UFD token: ' UFD_TOKEN
export UFD_TOKEN
python3 tools/send_frame.py waiting --passage cash-101 --plate 'AAA 000' --fare 200
python3 tools/send_frame.py paid --passage cash-101 --plate 'AAA 000' --fare 200
python3 tools/send_frame.py rfid-paid --passage rf-102 --plate 'XYZ 789' --fare 100 --balance 900
python3 tools/send_frame.py closed
python3 tools/send_frame.py open
```

Run payment states only after corresponding business events; an RFID read is not
payment approval. Unknown balance must not be sent as zero. Unknown LPR uses an
empty plate/omitted field; the display shows NO PLATE instead of the last vehicle.

## Compatibility limitation

The existing MewRF balance-as-`vehicle_number` format (for example `R.BAL900`) is not
real LPR data. Migrate it to the dedicated balance field and provide the current
vehicle's LPR plate. Otherwise the firmware cannot distinguish that text from a plate.
