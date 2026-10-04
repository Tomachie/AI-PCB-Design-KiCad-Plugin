# Builds the KiCad PCM package for tweb, and the three files needed to host it.
#
#     python make_package.py [version] [base_url]
#
# Every change to the package's content gets a new version number: two
# different files both named ...-1.1.0.zip cannot be told apart in a Downloads
# folder, and KiCad will not offer the newer one as an update.
#
# Inputs  : out/tweb.exe (build.bat), plus the files in this directory and pcm/.
# Outputs : build/<identifier>-<version>.zip   the package
#           build/repository.json              the URL users add in PCM
#           build/packages.json                the package listing
#           build/resources.zip                icons PCM shows in the list
#
# The package archive contains:
#     metadata.json        at the root
#     plugins/...          extracted to <docs>/KiCad/<ver>/3rdparty/plugins/<identifier>/
#     resources/icon.png   64x64, shown in the PCM list

import copy
import hashlib
import json
import os
import shutil
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "out", "tweb.exe")

# The schema KiCad itself checks a package against, shipped with KiCad. Every
# file this tool writes is validated against it before anything lands in
# build/, so a package KiCad would refuse ("Unable to parse package metadata")
# is never built.
PCM_SCHEMA = r"C:\Program Files\KiCad\10.0\share\kicad\schemas\pcm.v2.schema.json"

try:
    import jsonschema
except ImportError:
    sys.exit("jsonschema is needed to check the package against KiCad's schema:\n"
             "  python -m pip install --user jsonschema")
if not os.path.exists(PCM_SCHEMA):
    sys.exit("KiCad's package schema not found (is KiCad 10 installed?):\n  " + PCM_SCHEMA)
pcm_schema = json.load(open(PCM_SCHEMA, encoding="utf-8"))


def schema_errors(definition, document, label):
    """Errors of one document against one definition of KiCad's PCM schema."""
    validator = jsonschema.Draft7Validator(
        {"$ref": "#/definitions/" + definition, "definitions": pcm_schema["definitions"]})
    return ["%s /%s : %s" % (label, "/".join(str(p) for p in e.absolute_path), e.message)
            for e in validator.iter_errors(document)]


version = sys.argv[1] if len(sys.argv) > 1 else "1.2.0"
base_url = sys.argv[2] if len(sys.argv) > 2 else "https://tomachie.com/kicad"

build = os.path.join(HERE, "build")
stage = os.path.join(build, "stage")
if os.path.isdir(build):
    shutil.rmtree(build)
os.makedirs(os.path.join(stage, "plugins", "i18n"))
os.makedirs(os.path.join(stage, "resources"))

if not os.path.exists(EXE):
    sys.exit("not found - run build_tweb.bat first:\n  " + EXE)

# --- package contents -------------------------------------------------------
shutil.copy(EXE, os.path.join(stage, "plugins", "tweb.exe"))
for f in ("tweb.json", "plugin.json", "LICENSE",
          "icon-light-24.png", "icon-light-48.png",
          "icon-dark-24.png", "icon-dark-48.png"):
    shutil.copy(os.path.join(HERE, f), os.path.join(stage, "plugins", f))
shutil.copy(os.path.join(HERE, "i18n", "tweb_en.txt"),
            os.path.join(stage, "plugins", "i18n", "tweb_en.txt"))
# every translated strings file ships too, once they exist
for f in sorted(os.listdir(os.path.join(HERE, "i18n"))):
    if f.startswith("tweb_") and f.endswith(".txt"):
        shutil.copy(os.path.join(HERE, "i18n", f),
                    os.path.join(stage, "plugins", "i18n", f))
shutil.copy(os.path.join(HERE, "pcm", "icon.png"),
            os.path.join(stage, "resources", "icon.png"))

meta = json.load(open(os.path.join(HERE, "pcm", "metadata.json"), encoding="utf-8"))
meta["versions"][0]["version"] = version
problems = schema_errors("Package", meta, "pcm/metadata.json")
if problems:
    shutil.rmtree(build)
    sys.exit("KiCad would refuse this package - nothing was built:\n  " + "\n  ".join(problems))
json.dump(meta, open(os.path.join(stage, "metadata.json"), "w", encoding="utf-8"), indent=2)

# --- the archive ------------------------------------------------------------
pkg_name = "%s-%s.zip" % (meta["identifier"], version)
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

# --- hosting: repository.json + packages.json + resources.zip ---------------
listing = copy.deepcopy(meta)
listing.pop("$schema", None)
v = listing["versions"][0]
v["download_url"] = "%s/%s" % (base_url, pkg_name)
v["download_sha256"] = hashlib.sha256(blob).hexdigest()
v["download_size"] = len(blob)
v["install_size"] = install_size

packages_path = os.path.join(build, "packages.json")
json.dump({"packages": [listing]}, open(packages_path, "w", encoding="utf-8"), indent=2)

resources_path = os.path.join(build, "resources.zip")
with zipfile.ZipFile(resources_path, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(stage, "resources", "icon.png"),
            "%s/icon.png" % meta["identifier"])


def stamp(path):
    data = open(path, "rb").read()
    now = int(time.time())
    return {"url": "%s/%s" % (base_url, os.path.basename(path)),
            "sha256": hashlib.sha256(data).hexdigest(),
            "update_timestamp": now,
            "update_time_utc": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(now))}


# schema_version 2 is what makes KiCad check packages.json against its v2
# schema. Without it KiCad assumes v1, whose licence list is a closed enum of
# open-source licences and refuses "proprietary".
repository = {
    "$schema": "https://go.kicad.org/pcm/schemas/v2#/definitions/Repository",
    "schema_version": 2,
    "name": meta["name"],
    "maintainer": meta["author"],
    "packages": stamp(packages_path),
    "resources": stamp(resources_path),
}
json.dump(repository, open(os.path.join(build, "repository.json"), "w", encoding="utf-8"),
          indent=2)

problems = (schema_errors("PackageArray", {"packages": [listing]}, "packages.json")
            + schema_errors("Repository", repository, "repository.json"))
if problems:
    shutil.rmtree(build)
    sys.exit("KiCad would refuse these hosting files - build/ removed:\n  " + "\n  ".join(problems))

shutil.rmtree(stage)

print("package      : build/%s  (%d bytes, installs to %d)" % (pkg_name, len(blob), install_size))
print("sha256       : %s" % v["download_sha256"])
print("host these   : build/repository.json, build/packages.json, build/resources.zip,")
print("               build/%s" % pkg_name)
print("users add    : %s/repository.json" % base_url)
print()
print("To test without hosting anything: KiCad > Plugin and Content Manager >")
print("Install from File, and pick build/%s" % pkg_name)
