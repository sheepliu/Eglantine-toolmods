# Item Add Runtime

This self-contained tool-mod package targets the analyzed Fortune's Weave
build. It owns its manifest, catalog, and runtime entrypoint. The generic
host discovers the package, validates its manifest, substitutes request
placeholders, and launches the declared runtime. Game-specific IID lookup and
inventory commit logic stay in `itemadd_gdb.py`.

The host substitutes `{entrypoint}`, `{package_dir}`, `{title_id}`, `{iid}`,
and `{quantity}` in the manifest argument template. Reports are written to
the package `reports` directory. The Eglantine GDB log is read from its normal
per-user location.

The catalog includes Japanese, Traditional Chinese, and Simplified Chinese
names. The Chinese labels are joined from the game's `TWzh` and `CNzh` Item
message tables by the shared `MIID_*` label, so searching `伤药` resolves the
same IID as `傷藥` and `傷薬`.

Indirect workflow:

1. Start Eglantine with the guest GDB Stub on port `6543`.
2. Open `Runtime Tool Mods`, enable this package, press Save, reopen the
   dialog, and open its item catalog.
3. Select an item and quantity, press `Add to Convoy`, then trigger one normal
   purchase or pickup. The entrypoint commits the selected item in that valid
   inventory context and resumes the game.

The package is pinned to the analyzed `main` SHA-256 and Build ID. `direct`
mode is intentionally not used by the UI because a cached guest context is
one-shot and not repeat-safe.
