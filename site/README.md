# Bridge Explorer

A Chinese/English interactive explanation of the BL616CL bridge for UNO R4 WiFi.
The default language is Simplified Chinese; `?lang=en` selects English. Lesson
links use fragments, for example `?lang=en#ble` and `#matrix`.

Public site: <https://bouffalolab.github.io/arduino-bouffalo-uno-r4-wifi-usb-bridge/>

## Preview and check

From the repository root:

```sh
python3 -m http.server 8765 --bind 127.0.0.1 --directory site
```

Open <http://127.0.0.1:8765/> in a current browser. The site uses plain HTML, CSS,
SVG and ES modules, with no build step, npm dependencies, remote fonts, analytics,
CDN or backend. Serve it over HTTP rather than opening `index.html` as a file.
Viewed-lesson IDs are stored in this site's browser localStorage; no data is sent
to a service. GitHub Pages handles ordinary hosting requests.

Run auxiliary content checks with Node.js 22 or newer:

```sh
node --test site/tests/*.test.js
for source in site/assets/*.js; do node --check "$source"; done
```

The checks compare the matrix presets against `PhoneMatrix.ino`, verify row-major
MSB-first packet packing and declared payload lengths, and check bilingual data,
publication warnings, security boundaries and asset paths. They do not test
firmware, a radio, TLS security or a physical board.

For a repeatable browser check, install `agent-browser`, launch the preview, then:

```sh
agent-browser --session bridge-site open http://127.0.0.1:8765/
agent-browser --session bridge-site set viewport 1600 1000
agent-browser --session bridge-site wait --load networkidle
agent-browser --session bridge-site eval --stdin < site/tests/browser-check.js
agent-browser --session bridge-site set viewport 390 844
agent-browser --session bridge-site eval --stdin < site/tests/browser-check.js
agent-browser --session bridge-site errors
```

Also inspect screenshots of the board, BLE matrix editor and support table. A
mobile viewport is a responsive-layout check, not a real-phone hardware test.
The browser test is run locally, not by the Pages workflow.

## Maintain the content

- `assets/content.js`: both languages, lesson order, links and replay steps.
- `assets/board.js`: original rotatable isometric SVG illustration and animations.
- `assets/protocol.js`: display escaping and the PhoneMatrix raw-frame encoder.
- `assets/app.js`: navigation, tabs, localization, component details and replays.
- `assets/style.css`: responsive layout, focus styles and reduced-motion handling.

The bridge [README](../README.md), [DEVELOP.md](../DEVELOP.md),
[PROTOCOL.md](../docs/PROTOCOL.md), [example guide](../examples/README.md),
[PhoneMatrix source](../examples/PhoneMatrix/PhoneMatrix.ino) and platform
[SUPPORT.md](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/SUPPORT.md)
and [release notes](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/RELEASE-NOTES.md)
are the content authorities. AT examples were checked against `at_server.cpp`
and Renesas UNO 1.6.0 / WiFiS3; HCI behavior also follows ArduinoBLE 2.1.0.
Content was checked on November 5, 2026. Update both languages when those contracts
or release states change; do not turn experimental hardware results into a
supported-feature claim. Website deployment does not publish firmware or approve
a package index.

The replays are authored illustrations, **not captured hardware logs**. The matrix
editor encodes the real example's protocol but never connects to a board. No
Arduino compiler, CPU emulator, Web Serial or Web Bluetooth is included. Code
panels are snippets, not complete projects; follow the repository build guide.

## Deploy

The `bridge-explorer-pages` workflow checks the site on relevant pull requests.
On relevant pushes to `master`, or a manual run on `master`, it checks and stages
only `index.html` and `assets/`, uploads a Pages artifact and deploys through the
`github-pages` environment. Tests and this maintainer guide are not deployed.
Existing source and firmware workflows are unchanged.

Repository **Settings → Pages → Build and deployment → Source** must be set to
**GitHub Actions**. After a deployment, check both the Chinese page and
`?lang=en#matrix` at the public URL. The existing firmware workflows and release
approval remain separate.

## Sources, drawing and license

The drawing's board outline and major component positions were measured from the
[official ABX00087 CAD package](https://docs.arduino.cc/resources/cad/ABX00087-cad-files.zip),
with the [official pinout](https://docs.arduino.cc/resources/pinouts/ABX00087-full-pinout.pdf)
and the board schematic as references. The BL616CL module and internal 1V0 wiring
follow the bridge sources. Small passives are omitted; heights, module appearance
and colored logical paths are illustrative, not manufacturing CAD or actual PCB
tracks. No upstream CAD, diagram, font, model, screenshot or branding asset is
redistributed in the site.

The course format was inspired by [learn-arduino by Dilum Sanjaya](https://learn-arduino-red.vercel.app/).
This site's graphics, implementation and bridge-specific content are original.
Site source follows the repository's [Apache-2.0 license](../LICENSE). Arduino and
UNO are trademarks of Arduino SA; this project is not affiliated with Arduino.
