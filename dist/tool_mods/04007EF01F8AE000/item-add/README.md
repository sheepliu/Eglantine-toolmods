# Item Add Runtime

This self-contained Fortune's Weave package owns its item catalog and runtime
entrypoint. The generic ToolMods host launches it through the manifest; IID
lookup and inventory commit logic remain in `itemadd_gdb.py`.

The v1.0.2 distribution includes `python-runtime` beside `eglantine.exe`.
Keep that directory and `tool_mods` together. System Python installation and
PATH setup are not required.

1. Start Eglantine, enable the guest GDB Stub on port `6543`, and start the game.
2. Open `Runtime Tool Mods`, enable Item Add, and open its management page
   using the action on the mod's row.
3. Search by Japanese, Traditional/Simplified Chinese name or IID. Select an
   item and quantity, press `Add to Convoy`, then buy or pick up one normal
   item in the game. That event supplies the inventory context for the request.
4. Use `Cancel` to withdraw a pending request before its inventory event.

The complete catalog contains 1,355 IIDs, including examination certificates
and upgraded weapons. See [catalog details](CATALOG.md) for source evidence,
fallback names, and exact examination-item IDs. Return to the mod list and
reopen this page after updating `items.json`.

Reports are written to the package's `reports` directory, with process output
in `runtime.log`. The runtime uses the emulator's per-user log to locate the
guest main module. It checks the known game build before mutation.

Target Title ID: `04007EF01F8AE000`; main SHA-256:
`616e7d906936ea4c2d908d0553cf69779e60242d769c40722267ee8bcb98e79a`.
The UI uses the indirect inventory-event mode. Direct calls through a cached
guest context are not repeat-safe and are not used by this interface.
