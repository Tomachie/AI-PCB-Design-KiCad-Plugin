# Submission to KiCad's official add-on repository

`com.tomachie.kicad/` is the folder to add under `packages/` in
https://gitlab.com/kicad/addons/metadata (fork, add the folder, open a merge
request). `metadata.json` points at the GitHub release asset for v1.2.0 and
carries its SHA-256 and size; it passes KiCad's pcm.v2 schema.

On each new release, update `versions` in `metadata.json` (download_url,
download_sha256, download_size, install_size) from the release's packages.json.
