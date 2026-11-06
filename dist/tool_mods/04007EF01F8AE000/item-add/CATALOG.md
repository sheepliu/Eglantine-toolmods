# Item catalog — complete table export (2026-11-06)

The catalog contains 1,355 distinct IIDs from this game's Item.bin. Only
`IID_無し`, the null item sentinel, is omitted. The previous 580-entry export
matched Japanese message text against IID suffixes and missed renamed items,
upgrades, and event/internal variants. This export follows each actual item
record's name-label pointer instead.

- 1,262 entries use JPja/TWzh/CNzh names joined by the record's MSBT label.
- 399 upgrade entries share base-weapon name labels; their explicit IID
  enhancement suffix is appended to each displayed name as `+1`, etc.
- 93 entries have no name label. They remain available with an explicit
  `[无官方名称]` / `[無官方名稱]` / `[名称なし]` prefix and their IID suffix.
- All old IIDs remain present. The `IID_魔響石・大` display name is corrected
  to `魔响石·特大`, following its actual `MIID_ResonateStoneL` label.

Examples:

| Simplified Chinese | IID | Item index |
| --- | --- | --- |
| 初级考试票证 | IID_下級資格手形 | 1125 |
| 中级考试票证 | IID_中級資格手形 | 1126 |
| 上级考试票证 | IID_上級資格手形 | 1127 |
| 最上级考试票证 | IID_最上級資格手形 | 1128 |
| 战象兵考试票证 | IID_戦象兵資格手形 | 1129 |
| 舞姬的首饰 | IID_踊り子資格手形 | 1130 |

Search `考试` to find the examination items. The text resources also contain
`MIID_TankLicense` (战车兵考试票证), but this Item.bin has no item record
referencing it; no IID was invented for that unused message.

This is a catalog update only. It does not change the emulator, loader,
manifest runtime path, guest hook, or inventory-commit behavior. Close the
catalog page and reopen it from the mod list to reload items.json.

Row identities and names have been checked offline. Individual grant behavior
is not proven for every item, particularly internal/action/demo variants.

Source Item.bin SHA-256:
`0895a35581369306623749f77d00701b12e98f4427ec9123b3edd3b84ab472a7`.

Catalog SHA-256:
`A08868AC162DFCF13BF3106258D5016E7D8DBD93300B6B18A601B3B940B86AB9`.
