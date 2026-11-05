# Friendship Adjust Runtime

This package waits for one normal gift transaction in Fortune's Weave, then
replaces the relationship-point delta at the game's own `GameReliance::IData`
score writer. The original writer remains responsible for bounds, level
changes, UI notifications, and save persistence.

The package targets main SHA-256
`616e7d906936ea4c2d908d0553cf69779e60242d769c40722267ee8bcb98e79a` and Build
ID `DB93B0125F4850601D61C3EDDE012F8EB2ECFE77000000000000000000000000`.

The package uses the generic `numeric_action` page in the host UI. Enter the
desired positive amount in `Next gift increase`, press `Apply to next gift`,
and then complete one normal gift in-game. The selected value is passed as
`--amount` to the runtime script.

The legacy `characters.json` catalog remains in the package as analysis data.
It is not used by the fixed-delta action.

```powershell
python -X utf8 friendship_gdb.py --amount 500 --wait 600
```

Start Eglantine with GDB Stub on port `6543`, reach the game, run the command,
and complete one normal gift. Reports are written to `reports/` when that
directory is supplied by the host, or to `work/friendship_probe/` by default.

No guest memory or save bytes are written directly; only the score-writer
argument register is changed at its game-owned entry point.

The package includes `itemadd_gdb.py` as a private GDB-RSP helper dependency;
it does not require the development workspace or the ItemAdd package after
installation.

The catalog is generated from the three locale-aligned `Person.msbt` files and
the relationship table. `GameData/Reliance.bin` currently contains eight
confirmed relationship candidates: `MPID_Kai`, `MPID_Ditlih`,
`MPID_Seodola`, `MPID_Leda`, `MPID_Beltran`, `MPID_Talimun`, `MPID_Orhel`,
and `MPID_Anhatolyah`. This is a static table candidate list, not proof that
the live `IData` pointer has been mapped to each ID.
The catalog also records the static table order and Person short IDs (`Kai`,
`Dit`, `Seo`, `Led`, `Bel`, `Tal`, `Orh`, `Anh`) for those eight entries.

For live recipient mapping, use the separate read-only scanner from the
development workspace:

```powershell
python -X utf8 tools\friendship_scan_gdb.py --count 1 --wait 600
```

Each event records the `IData` pointer, type, level, score, related pointers,
and bounded memory windows. Capture one gift for each recipient and compare the
reports before assigning character IDs. The scanner never changes registers,
guest memory, saves, or the original writer.
