# Tomachie for KiCad

A button in KiCad's schematic editor that sends the open design to
[Tomachie](https://tomachie.com) for design-for-test analysis: checks that go
deeper than ERC, test-point insertion, and an AI design review. The checks and
test-point insertion are free, and so is the AI review for one-page designs.
You choose the options in your browser, and the results are emailed to you.

**Requirements:** Windows, KiCad 10.0 or later. A Linux build is planned.

## Install

Through KiCad's **Plugin and Content Manager**:

- **Install from File:** download the package from
  <https://tomachie.com/en/kicad-plugin.html>, then Plugin and Content Manager
  → *Install from File…* → choose the zip (do not unzip it) → restart KiCad.
- **Or add our repository**, which brings updates automatically: Plugin and
  Content Manager → *Manage…* → add `https://tomachie.com/kicad/repository.json`
  → install **Tomachie**.

The plugin uses the KiCad API. If Preferences → Plugins → *Enable KiCad API*
is off, KiCad offers to switch it on when the plugin is installed. While it is
off, no plugin button appears.

The Tomachie check mark is at the right-hand end of the schematic editor's top
toolbar. To move it, for example next to the Electrical Rules Checker:
Preferences → Schematic Editor → Toolbars → *Top main* → drag
*IPC/Scripting plugins*.

## What it sends, and when

1. It asks KiCad over the KiCad API which schematic is open.
2. It finds the project file in KiCad's list of open projects and follows the
   sheet hierarchy from the root sheet.
3. It zips **only** the schematic sheets the design references (`.kicad_sch`),
   the project file (`.kicad_pro`) and the design rules (`.kicad_dru`). No PCB
   layout, Gerbers, 3D models, backups, `.history` or other files in the
   folder.
4. On the first run only, a dialog asks for the email address the results go
   to, and links to the
   [Confidential Disclosure Agreement and Privacy Policy](https://tomachie.com/en/schematic-design-review-cda.html).
5. It uploads the zip over HTTPS to `https://tomachie.com/stage` and opens the
   page the server returns in your browser.

**Nothing is analysed until you press Analyze on that page.** A staged upload
that is not submitted is deleted after two hours. The server is the only place
the analysis runs. This client contains no analysis.

Save the schematic before clicking: KiCad gives a plugin no way to save for you
or to detect unsaved edits, so the plugin sends what is on disk.

The plugin stores one file of its own, `%APPDATA%\Tomachie\tweb_user.json`,
holding the email address. Hold **Shift** while clicking the button to change
it. It sends no telemetry.

## Build

Visual Studio 2022 (MSVC, C++17), no other dependencies. KiCad's own `nng.dll`
(API transport) and `zlib1.dll` (zip compression) are loaded at run time.

```
build.bat                           builds out\tweb.exe
python make_package.py 1.2.0        builds the KiCad package into build\
```

`make_package.py` checks every package file against the schema KiCad
ships (`share\kicad\schemas\pcm.v2.schema.json`) and builds nothing if one
fails. It needs `python -m pip install jsonschema`.

Offline tests, no KiCad needed:

```
tweb.exe --collect <project_dir> <root_sheet.kicad_sch>   package only
tweb.exe --stage   <archive.zip>                          upload only
tweb.exe --settings                                       open the dialog
```

Each run is logged to `tweb_log.txt` beside `tweb.exe`.

## Files

| File | Purpose |
|---|---|
| `tweb.cpp` | The client: KiCad API, project resolution, sheet walk, zip writer, upload, first-run dialog |
| `string_table.h/.cpp` | Dialog strings (`i18n/tweb_<lang>.txt`) |
| `plugin.json` | KiCad plugin manifest |
| `tweb.json` | Paths on tomachie.com the client uses |
| `pcm/metadata.json` | Plugin and Content Manager package description |
| `tweb.rc`, `tweb.ico`, `icon-*.png` | Icons and version resources |
| `build.bat`, `make_package.py` | Build the exe; build and check the KiCad package |
| `extract_proto.py` | Extracts KiCad's protobuf descriptors from `kiapi.dll`, to re-check field numbers when KiCad changes version |

## Licence

MIT. See [LICENSE](LICENSE). Tomachie's analysis service is operated by
Tomachie LLC and is not part of this repository.
