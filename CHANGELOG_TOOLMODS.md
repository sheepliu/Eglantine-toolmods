# ToolMods changes

## 1.0.2

- Resolve path-valued package runtimes relative to the mod directory for both
  catalog and numeric-action UI launch paths. Bare runtime names retain their
  existing PATH lookup behavior.
- Bundle official CPython 3.13.12 amd64 embeddable runtime as `python-runtime/`.
  Both runtime mods use it without requiring system Python installation.
- Expand ItemAdd from 580 to 1,355 entries by following Item.bin name pointers.
  Add examination certificates, 399 upgraded weapons, and previously omitted
  event/internal variants. Preserve all old IIDs; exclude only the null sentinel.
- Mark the 93 entries without official names and retain upgrade suffixes in
  displayed names. Correct the actual name mapping for `IID_魔響石・大`.
- Retain the tested Friendship Adjust UI, gift-triggered fixed delta, and
  cooperative cancellation. The fixed-delta mod still expects the unpatched
  score-writer entry; disable Friendship multiplier IPS when using it.
- Verify catalog hash, bundled-runtime version, both entrypoints with system
  Python absent from PATH, and actual Release compiler optimization commands.
- Keep one canonical ToolMods integration on build-194 and generate all release
  channels from it. Build-194, build-212 and RC packages use the same mod data
  and loader changes, with separate upstream provenance in VERSION.json.

Installation: extract the entire overlay ZIP into a complete Windows x64
Eglantine installation of the matching upstream release. Retain its Qt DLLs
and plugins. `eglantine.exe`, `tool_mods/`, and `python-runtime/` belong together.
User settings and saves remain in the existing user-data directory.

Local validation: build-212 with this runtime resolver and bundled Python
passed user gameplay tests without system Python on PATH and without reported
stutter. Newly added individual catalog entries have offline ID/name validation;
not every item has been granted in game. CI uses MSVC/dynamic Qt, while the local
tested build used clang-cl/static Qt; generated release binaries need gameplay
smoke testing after download.
