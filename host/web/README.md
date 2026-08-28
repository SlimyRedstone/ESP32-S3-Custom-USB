# Web client

Browser control panel for the ESP32-S3 composite device (`303A:4001`), over
WebUSB. Colour wheel for the NeoPixel, text box for messages, live traffic log.

## Running

```sh
cd host/web
python -m http.server 8000
```

Open <http://localhost:8000> in Chrome or Edge and press **Connect**.

WebUSB requires a secure context. `localhost` counts, `file://` is unreliable, so
serve it rather than double-clicking `index.html`.

## What it sends

The wire protocol is JSON, one object per bulk transfer.

| Control | Packet |
| --- | --- |
| Colour wheel, brightness slider, hex box | `{"set":{"led":"RRGGBB"}}` |
| Message box | `{"set":{"message":"<text>"}}` |
| Config **Set** | `{"set":{"config":{...}}}` |
| LED **Get** | `{"get":"led"}` |
| Config **Get** | `{"get":"config"}` |

Transfers larger than 64 bytes are reassembled by the firmware, so payloads are
bounded by its 512-byte JSON buffer rather than by the endpoint size.

## What it shows

- `{"ok":true}` / `{"ok":false,"error":"..."}` replies to every `set`
- `{"led":"ABCDEF"}` from a get — the wheel snaps to the reported colour
- `{"config":{...}}` from a get — fills the config editor
- `{"interrupt":{"gpio":0,"state":0,"message":"..."}}` when GPIO0 is pressed
- Heartbeat counters, hidden by default — tick **Show heartbeats**

## Notes

Only one process may hold the vendor interface at a time. Close
`../vendor_test.py` before connecting from the browser, and vice versa.

The CDC function is untouched by this page, so a terminal on the COM port can
stay open alongside it. That is where the text from `{"set":{"message":...}}`
actually appears — this page only confirms that the command was accepted.

Browser support is Chrome and Edge. Firefox and Safari have no WebUSB. On
Windows the vendor function needs WinUSB bound to it, which the firmware's
MS OS 2.0 descriptors arrange automatically.
