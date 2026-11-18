# Licences

PhishTap Lite is a college demo project. Everything in `assets/libs/` is
third-party and vendored, because the board may have no internet at demo time and
`docs/ARCHITECTURE.md` §7 forbids external requests from the pages.

| File | Upstream | Licence | Size |
|---|---|---|---|
| `assets/libs/socket.io.min.js` | Socket.IO client v4.8.1 | MIT | 46,788 B |
| `assets/libs/arduino.js` | Arduino App Lab `web_ui` brick helper | MPL-2.0 | 1,392 B |
| `assets/libs/qrcode.min.js` | davidshimjs/qrcodejs | MIT | 19,927 B |

## Provenance

All three were pulled **from the target board itself**, out of the Arduino App Lab
SDK's own bundled examples, rather than downloaded from the internet:

```
/var/lib/arduino-app-cli/examples/bricks/arduino/web_ui/05_send_message/assets/libs/socket.io.min.js
/var/lib/arduino-app-cli/examples/bricks/arduino/web_ui/05_send_message/assets/libs/arduino.js
/var/lib/arduino-app-cli/examples/inspirational/common/mobile-remote-sensor/assets/libs/qrcode.min.js
```

Reproduce with:

```bash
ADB=~/Android/Sdk/platform-tools/adb
E=/var/lib/arduino-app-cli/examples/bricks/arduino/web_ui/05_send_message/assets/libs
$ADB pull $E/socket.io.min.js assets/libs/
$ADB pull $E/arduino.js       assets/libs/
$ADB pull /var/lib/arduino-app-cli/examples/inspirational/common/mobile-remote-sensor/assets/libs/qrcode.min.js assets/libs/
```

This matters for two reasons. The Socket.IO **client and server versions must be
compatible** — the board runs `python-socketio` 5.17.0 behind `fastapi-socketio`
0.0.10, and the client Arduino ships alongside that brick is the one they test
together. Guessing a client version from a CDN is how risk R4 in
`docs/ARCHITECTURE.md` happens. And a file pulled from the SDK has unambiguous
provenance, which a file fetched from a URL does not.

`qrcode.min.js` is byte-identical to Arduino's copy except for a prepended
licence banner. The minified original carries no header, and MIT requires the
notice to travel with the file, so it was added. `tools/` verifies the code bytes
after the banner are unchanged.

## Deviation from the architecture document

`docs/ARCHITECTURE.md` §7 originally specified nayuki/QR-Code-generator and
rejected qrcodejs as "unmaintained since 2015". That decision was reversed, for
reasons worth recording rather than quietly editing away:

- qrcodejs is **already on the target device**, shipped by Arduino in their own
  examples, so it is the version they test against.
- Generating a QR code from a URL is a frozen problem. ISO/IEC 18004 has not
  changed, the library runs client-side, it produces an image and takes a string.
  There is no network surface and no credential handling, so "unmaintained"
  carries close to no risk here — unlike, say, an unmaintained TLS library.
- Using it removes a download step, a supply-chain question and a licence
  investigation, all of which are pure risk for a college demo.

nayuki's generator remains a fine choice and would be preferred for a project
where the QR content or the rendering path were more complex.

---

## Socket.IO client v4.8.1 — MIT

```
The MIT License (MIT)

Copyright (c) 2014-2024 Guillermo Rauch

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## qrcodejs — MIT

```
The MIT License (MIT)

Copyright (c) 2012 davidshimjs

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## arduino.js — MPL-2.0

```
SPDX-FileCopyrightText: Copyright (C) Arduino s.r.l. and/or its affiliated companies
SPDX-License-Identifier: MPL-2.0
```

Mozilla Public License 2.0. Full text: https://www.mozilla.org/en-US/MPL/2.0/

MPL-2.0 is file-scoped copyleft: the obligations attach to `arduino.js` itself,
not to the rest of this project. The file is included unmodified apart from
nothing — it is vendored verbatim. Note that this project does **not** actually
use `arduino.js`; it is kept for reference because it documents the brick's
WebSocket conventions, and `assets/js/common.js` implements the same protocol
directly. Two reasons it is not used as-is:

- It hardcodes `io(\`http://${window.location.host}\`)`, which breaks when the
  brick runs with `use_tls=True` and the page is served over HTTPS. Browsers
  block mixed content, so the socket would never connect.
- It exposes no way to join a Socket.IO room, and this project needs the phone to
  join a room named for its session token.

It may be deleted if that reference value is not wanted; nothing imports it.

## First-party code

Everything outside `assets/libs/` is original to this project. A licence for it is
to be chosen at Stage 5.
