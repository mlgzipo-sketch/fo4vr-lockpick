# VRLockpick for Fallout 4 VR

Pick locks with your own hands in Fallout 4 VR instead of the thumbsticks. The game's own lock, bobby pin and screwdriver models and its lockpicking rules (sweet spot, skill, perks, broken pins) are kept; your hands drive them.

Work in progress. Current build: the bobby pin follows the roll of your pick hand, and the plugin logs what the lockpicking menu does.

## Requirements

- Fallout 4 VR 1.2.72
- [F4SE VR](https://f4se.silverlock.org/)
- VR Address Library for F4SEVR
- [FRIK](https://www.nexusmods.com/fallout4/mods/53464)
- [ROCK](https://github.com/brunocatani/ROCK) 0.9 or newer

## Installation

Download the `VRLockpick` archive from the latest successful run under Actions and install it with Vortex or MO2. You should end up with:

```
Data\F4SE\Plugins\VRLockpick.dll
Data\F4SE\Plugins\VRLockpick.ini
```

Log: `Documents\My Games\Fallout4VR\F4SE\VRLockpick.log`.

## Building

Built by GitHub Actions (windows-2022, CMake, vcpkg, [CommonLibF4VR](https://github.com/ArthurHub/CommonLibF4VR), [RPS SDK](https://github.com/brunocatani/RPS_SDK)).

## License

GPL-3.0.
