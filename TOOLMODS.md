# Eglantine Tool Mods

This fork keeps the generic Tool Mod host in the emulator and ships the
ItemAdd and Friendship Adjust as external per-title packages.

## Branches

- `toolmods/build-194`: known-good pinned baseline for the original build-194
  runtime and the canonical ToolMods integration branch.
- `toolmods/test-build`, `toolmods/test-rc`, and `toolmods/stable`: generated
  channels rebased onto the corresponding upstream Release tags. They are
  regenerated from the canonical baseline after every push to
  `toolmods/build-194`, and also by the scheduled/manual upstream sync. The
  stable channel is skipped until upstream publishes a non-prerelease Release.
- `origin`: the upstream `Djihads80/Eglantine` repository.

## Local package layout

Place `tool_mods` beside `eglantine.exe`. The host discovers packages only from
the executable directory, under each game's title ID:

```text
eglantine.exe
python-runtime\python.exe
tool_mods\04007EF01F8AE000\item-add\
tool_mods\04007EF01F8AE000\friendship-adjust\
```

The package contains `manifest.json`, `items.json`, `itemadd_gdb.py`, and
`README.md`. It is independent of the emulator executable after the generic
host has been compiled in. Enabled/disabled settings remain in the user's
`tool_mods.ini`; the previous `%APPDATA%\eden\config\tool_mods` package
directory is not scanned or migrated automatically.

Version 1.0.2 includes CPython 3.13.12 amd64 embeddable runtime. Both manifests
use `../../../python-runtime/python.exe`, resolved from the package directory
by the generic host. System Python and PATH changes are not required. Retain
`python-runtime` and `tool_mods` together beside the EXE when updating.
See [the changes and installation notes](CHANGELOG_TOOLMODS.md).

## CI behavior

- `toolmods-windows.yml` verifies the 1,355-entry catalog, package runtime
  manifests, bundled Python and both entrypoints before building. It checks
  actual Release optimization commands before compilation, then uploads the
  EXE, both runtime mods, Python runtime, and provenance in an overlay ZIP.
- `sync-upstream.yml` runs after every push to `toolmods/build-194`, daily, and
  on demand. It selects the newest numbered build and RC releases, plus an
  official stable release when one exists. Each channel starts from the same
  canonical ToolMods baseline, rebases it onto its upstream tag, and dispatches
  the Windows build only when its contents or upstream release changed.
  Conflicted channels are skipped without publishing a new artifact.
- `toolmods-windows.yml` builds only when dispatched. A successful build uploads
  an artifact and publishes the matching test prerelease or stable Release.
- Windows C/C++ compilation uses a shared sccache GitHub Actions cache. Initial
  cache population still compiles the full tree; later builds can reuse matching
  compiler outputs. Each build prints sccache hit/miss statistics in its log.

The CI toolchain remains MSVC with dynamic Qt 6.8.3; overlay ZIPs must be
extracted into a complete matching upstream installation with its Qt DLLs
and plugins. Build-194 is the canonical integration baseline and can be
published manually with channel `test-build`, upstream_ref `build-194`.
Automatic sync produces the newest numbered build and RC channels.

Every artifact records its upstream tag and commit, Tool Mods commit, title ID,
and game `main` SHA-256 in `VERSION.json`. A successful compile is not a runtime
compatibility proof; the target game boot and ItemAdd inventory flow remain a
manual smoke-test gate before distributing a release.
