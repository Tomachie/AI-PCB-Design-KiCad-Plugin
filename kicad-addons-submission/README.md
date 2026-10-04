# Submission to KiCad's official add-on repository

`com.tomachie.kicad/` is the folder to add under `packages/` in
https://gitlab.com/kicad/addons/metadata (fork, add the folder, open a merge
request). `metadata.json` points at the packages published on tomachie.com
(one version entry per platform, Windows and Linux) and carries their SHA-256
and sizes; it passes KiCad's pcm.v2 schema.

On each new release, update `versions` in `metadata.json` (download_url,
download_sha256, download_size, install_size) from the release's packages.json.
