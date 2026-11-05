# Eglantine Tool Mods

This fork keeps the generic Tool Mod host in the emulator and ships the
ItemAdd package as an external per-title package.

## Branches

- `toolmods/build-194`: known-good pinned baseline for the original build-194
  runtime.
- `toolmods/test-build`, `toolmods/test-rc`, and `toolmods/stable`: generated
  channels rebased onto the corresponding upstream Release tags. The stable
  channel is skipped until upstream publishes a non-prerelease Release.
- `origin`: the upstream `Djihads80/Eglantine` repository.

## Local package layout

Place `tool_mods` beside `eglantine.exe`. The host discovers packages only from
the executable directory, under each game's title ID:

```text
eglantine.exe
tool_mods\04007EF01F8AE000\item-add\
```

The package contains `manifest.json`, `items.json`, `itemadd_gdb.py`, and
`README.md`. It is independent of the emulator executable after the generic
host has been compiled in. Enabled/disabled settings remain in the user's
`tool_mods.ini`; the previous `%APPDATA%\eden\config\tool_mods` package
directory is not scanned or migrated automatically.

## CI behavior

- `toolmods-windows.yml` builds the Windows executable and validates the 580
  entry catalog, including Simplified Chinese names, before uploading a ZIP
  artifact.
- `sync-upstream.yml` runs daily and on demand. It selects the newest numbered
  build and RC releases, plus an official stable release when one exists.
  Each channel rebases the integration onto its upstream tag and dispatches
  the Windows build only when its contents or upstream release changed.
  Conflicted channels are skipped without publishing a new artifact.
- `toolmods-windows.yml` builds only when dispatched. A successful build uploads
  an artifact and publishes the matching test prerelease or stable Release.
- Windows C/C++ compilation uses a shared sccache GitHub Actions cache. Initial
  cache population still compiles the full tree; later builds can reuse matching
  compiler outputs. Each build prints sccache hit/miss statistics in its log.

Every artifact records its upstream tag and commit, Tool Mods commit, title ID,
and game `main` SHA-256 in `VERSION.json`. A successful compile is not a runtime
compatibility proof; the target game boot and ItemAdd inventory flow remain a
manual smoke-test gate before distributing a release.
