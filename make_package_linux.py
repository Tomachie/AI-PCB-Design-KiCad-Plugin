# Builds the KiCad PCM package for the Linux tweb port.
#
#     sh build.sh
#     python make_package_linux.py [version]
#
# Inputs  : out/tweb (from build.sh), plugin.json, tweb.json, LICENSE,
#           icons, i18n/, pcm/icon.png, pcm/metadata.json
# Outputs : build/<identifier>-linux-<version>.zip   the package
#           (Install from File in KiCad's Plugin and Content Manager)
#
# The package archive contains:
#     metadata.json        at the root (platforms: ["linux"])
#     plugins/tweb         the Linux binary, mode 0755
#     plugins/...          tweb.json, plugin.json, LICENSE, icons, i18n/
#     resources/icon.png   64x64, shown in the PCM list
#
# Unlike upstream make_package.py this does NOT emit repository.json /
# packages.json hosting files: install the local build with Install from File.
# Needs jsonschema (python -m pip install --user jsonschema) and a KiCad
# install for its pcm.v2.schema.json.

import hashlib
import json
import os
import shutil
import sys
import zipfile

try:
    import jsonschema
except ImportError:
    sys.exit("jsonschema is needed to check the package against KiCad's schema:\n"
             "  python -m pip install --user jsonschema")

HERE = os.path.dirname(os.path.abspath(__file__))
UPSTREAM = HERE
BIN = os.path.join(HERE, "out", "tweb")

# KiCad's own copy: a distribution package, a system-wide Flatpak, or a
# per-user Flatpak.
SCHEMA_LOCATIONS = [
    "/usr/share/kicad/schemas/pcm.v2.schema.json",
    "/var/lib/flatpak/app/org.kicad.KiCad/current/active/files/share/kicad/schemas/pcm.v2.schema.json",
    os.path.expanduser("~/.local/share/flatpak/app/org.kicad.KiCad/current/active/files/share/kicad/schemas/pcm.v2.schema.json"),
]
found = [p for p in SCHEMA_LOCATIONS if os.path.exists(p)]
if not found:
    sys.exit("KiCad's package schema not found (is KiCad 10 installed?). Looked in:\n  "
             + "\n  ".join(SCHEMA_LOCATIONS))
PCM_SCHEMA = found[0]
pcm_schema = json.load(open(PCM_SCHEMA, encoding="utf-8"))


def schema_errors(definition, document, label):
    """Errors of one document against one definition of KiCad's PCM schema."""
    validator = jsonschema.Draft7Validator(
        {"$ref": "#/definitions/" + definition, "definitions": pcm_schema["definitions"]})
    return ["%s /%s : %s" % (label, "/".join(str(p) for p in e.absolute_path), e.message)
            for e in validator.iter_errors(document)]


version = sys.argv[1] if len(sys.argv) > 1 else "1.3.1"

build = os.path.join(HERE, "build")
stage = os.path.join(build, "stage")
if os.path.isdir(build):
    shutil.rmtree(build)
os.makedirs(os.path.join(stage, "plugins", "i18n"))
os.makedirs(os.path.join(stage, "resources"))

if not os.path.exists(BIN):
    sys.exit("not found - build tweb first:\n  " + BIN)

# --- package contents -------------------------------------------------------
shutil.copy(BIN, os.path.join(stage, "plugins", "tweb"))
os.chmod(os.path.join(stage, "plugins", "tweb"), 0o755)
shutil.copy(os.path.join(UPSTREAM, "tweb.json"), os.path.join(stage, "plugins", "tweb.json"))
# plugin.json is shared with Windows, where the program is tweb.exe.
plugin = json.load(open(os.path.join(HERE, "plugin.json"), encoding="utf-8"))
for action in plugin["actions"]:
    action["entrypoint"] = "tweb"
json.dump(plugin, open(os.path.join(stage, "plugins", "plugin.json"), "w", encoding="utf-8"), indent=2)
shutil.copy(os.path.join(UPSTREAM, "LICENSE"), os.path.join(stage, "plugins", "LICENSE"))
for f in ("icon-light-24.png", "icon-light-48.png",
          "icon-dark-24.png", "icon-dark-48.png"):
    shutil.copy(os.path.join(UPSTREAM, f), os.path.join(stage, "plugins", f))
for f in sorted(os.listdir(os.path.join(UPSTREAM, "i18n"))):
    if f.startswith("tweb_") and f.endswith(".txt"):
        shutil.copy(os.path.join(UPSTREAM, "i18n", f),
                    os.path.join(stage, "plugins", "i18n", f))
shutil.copy(os.path.join(UPSTREAM, "pcm", "icon.png"),
            os.path.join(stage, "resources", "icon.png"))

meta = json.load(open(os.path.join(UPSTREAM, "pcm", "metadata.json"), encoding="utf-8"))
meta["versions"][0]["version"] = version
meta["versions"][0]["platforms"] = ["linux"]
problems = schema_errors("Package", meta, "pcm/metadata.json")
if problems:
    shutil.rmtree(build)
    sys.exit("KiCad would refuse this package - nothing was built:\n  " + "\n  ".join(problems))
json.dump(meta, open(os.path.join(stage, "metadata.json"), "w", encoding="utf-8"), indent=2)

# --- the archive ------------------------------------------------------------
pkg_name = "%s-linux-%s.zip" % (meta["identifier"], version)
pkg_path = os.path.join(build, pkg_name)
install_size = 0
with zipfile.ZipFile(pkg_path, "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(stage):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(root, f)
            install_size += os.path.getsize(full)
            z.write(full, os.path.relpath(full, stage).replace(os.sep, "/"))

blob = open(pkg_path, "rb").read()

print("package      : build/%s  (%d bytes, installs to %d)" % (pkg_name, len(blob), install_size))
print("sha256       : %s" % hashlib.sha256(blob).hexdigest())
print()
print("To test without hosting anything: KiCad > Plugin and Content Manager >")
print("Install from File, and pick build/%s" % pkg_name)
