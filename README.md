# Traders Refuse Irrelevant Items

A native RE_Kenshi/KenshiLib mod that makes traders refuse items they do not
deal in.

Instead of every merchant buying almost anything the player drops into the
trade window, traders now look at what their shop actually stocks. A crossbow
seller wants crossbows and bolts, a bar wants food and drink, and unrelated junk
is refused with a short trader response.

## Features

- Uses the trader's vendor lists to decide what they buy.
- General Stores accept world-only items (like Fog Prince Heads or Leviathan Pearls)
- Refuses irrelevant player-sold items before the sale completes.
- Supports both right-click selling and drag/drop selling.
- Leaves normal trading alone when no vendor list can be found.
- Lets off-list food sell at reduced value.
- Lets crossbow sellers buy crossbow ammo.
- Prevents backpacks with contents from being sold in risky refusal cases.
- Includes basic race-aware refusal dialogue for vanilla races.

## Requirements

- Kenshi
- RE_Kenshi
- KenshiLib build environment

## Build

Set up the KenshiLib toolchain using the original KenshiLib documentation:

- [KenshiLib Examples](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples)
- [KenshiLib Examples dependencies](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)

This project follows the same requirements: Visual Studio with the `v100`
toolset, Release x64, KenshiLib, and Boost 1.60.

For the local helper script:
```powershell
powershell -ExecutionPolicy Bypass -File native\toolchain\KenshiLib_Examples\TradersRefuse\build.ps1
```
To build the dll files:
```powershell
powershell -ExecutionPolicy Bypass -File native\toolchain\KenshiLib_Examples\TradersRefuse\deploy.ps1
```
