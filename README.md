<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

> **Eglantine ToolMods** is a maintained integration fork of Eglantine nx2 with a
> generic runtime Tool Mod host and the inventory ItemAdd package for title
> `04007EF01F8AE000`.
>
> This fork tracks upstream numbered builds and release candidates as test
> packages, and publishes a stable package only when upstream creates a formal
> non-prerelease release. Windows packages are built and published by the
> scheduled `sync-toolmods-upstream` workflow. Download them from the
> [ToolMods Releases](https://github.com/sheepliu/Eglantine-toolmods/releases)
> page. Extract the ZIP as a whole and keep `tool_mods` beside `eglantine.exe`.

## ToolMods Distribution

- **Windows package:** [latest ToolMods release](https://github.com/sheepliu/Eglantine-toolmods/releases)
- **Test channels:** latest upstream `build-N` and latest semantic `-rcN` release.
- **Stable channel:** latest upstream release marked as non-prerelease; no stable package is generated before one exists upstream.
- **ItemAdd package:** `tool_mods/04007EF01F8AE000/item-add/`
- **Version record:** `VERSION.json` contains the channel, exact upstream
  release tag and commit, ToolMods commit, game title ID, and expected game
  `main` hash.
- **Update policy:** the scheduled sync checks upstream Releases once per day,
  rebases each available channel, and explicitly dispatches its Windows build.

The original Eglantine documentation below remains available for emulator
building, configuration, compatibility, and licensing details.

<h1 align="center">
  <br>
  <a href="https://git.eden-emu.dev/eden-emu/eden"><img src="https://github.com/user-attachments/assets/35031845-4bfa-4916-8bf5-bc3bce67d5e2" alt="Eglantine" width="200"></a>
  <br>
  <b>Eglantine</b>
  <br>
</h1>

<h4 align="center"><b>Eglantine</b> is a free and opensource (FOSS) Switch 2 emulator, derived from Eden, Yuzu and Sudachi - started by developer Camille LaVey.
It's written in C++ with portability in mind, with builds for Windows, Linux and macOS
</h4>

<p align="center">
    </a>
    <a href="https://discord.gg/jCCZ58VExk">
        <img src="https://img.shields.io/discord/1367654015269339267?color=5865F2&label=Eglantine&logo=discord&logoColor=white"
            alt="Discord">
    </a>
  
</p>

<p align="center">
  <a href="#compatibility">Compatibility</a> |
  <a href="#development">Development</a> |
  <a href="#building">Building</a> |
  <a href="#download">Download</a> |
  <a href="#support">Support</a> |
  <a href="#license">License</a>
</p>

## Compatibility

The emulator is capable of running Mario Kart World at decent/full speed depending on specs and configurations, and Splatoon Raiders at variable performances

A list of supported games will be available in future. Please be patient.

## Development

For development discussions, please join us on [Discord](https://discord.gg/jCCZ58VExk)

If you would like to contribute, we are open to new developers and pull requests. Please ensure that your work is of a high standard and properly documented. You can also contact any of the developers on Discord or Stoat to learn more about the current state of the emulator.

## Documentation

We have a user manual! See our [User Handbook](./docs/user/README.md).

## Building

See the [General Build Guide](docs/Build.md)

For information on provided development tooling, see the [Tools directory](./tools)

## Download

You can download the upstream Eglantine releases from [here](https://github.com/Djihads80/Eglantine/releases).
For this fork's ToolMods-enabled Windows packages, use the [ToolMods Releases](https://github.com/sheepliu/Eglantine-toolmods/releases).

## Support

We currently do not take in any donations for the time being

If you would prefer to support us in a different way, please join our [Discord](https://discord.gg/jCCZ58VExk)

## Star History

<a href="https://www.star-history.com/?repos=djihads80%2Feglantine&type=date&legend=bottom-right">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=djihads80/eglantine&type=date&theme=dark&legend=bottom-right" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=djihads80/eglantine&type=date&legend=bottom-right" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=djihads80/eglantine&type=date&legend=bottom-right" />
 </picture>
</a>

## License

Eglantine is licensed under the GPLv3 (or any later version). Refer to the [LICENSE.txt](https://github.com/Djihads80/Eglantine/blob/nx2/LICENSE.txt) file.
