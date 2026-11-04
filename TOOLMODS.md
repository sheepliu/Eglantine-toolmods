# Eglantine Tool Mods

This fork keeps the generic Tool Mod host in the emulator and ships the
ItemAdd package as an external per-title package.

## Branches

- `toolmods/build-194`: known-good pinned baseline for the original build-194
  runtime.
- `toolmods/nx2`: branch produced by the upstream sync workflow after rebasing
  the integration commit onto `Djihads80/Eglantine:nx2`.
- `origin`: the upstream `Djihads80/Eglantine` repository.

## Local package layout

Copy the contents of `dist/tool_mods` into the emulator configuration path:

```text
%APPDATA%\eden\config\tool_mods\04007EF01F8AE000\item-add\
```

The package contains `manifest.json`, `items.json`, `itemadd_gdb.py`, and
`README.md`. It is independent of the emulator executable after the generic
host has been compiled in.

## CI behavior

- `toolmods-windows.yml` builds the Windows executable and validates the 580
  entry catalog, including Simplified Chinese names, before uploading a ZIP
  artifact.
- `sync-upstream.yml` runs daily and on demand. It rebases the integration
  commit onto an upstream ref. A conflict fails the job and does not publish
  a branch or artifact. A successful rebase pushes `toolmods/nx2` and starts
  the Windows build workflow.

Every artifact records its source commit, Tool Mod revision, title ID, and
game `main` SHA-256 in `VERSION.json`. A successful compile is not a runtime
compatibility proof; the target game boot and ItemAdd inventory flow remain a
manual smoke-test gate before distributing a release.
