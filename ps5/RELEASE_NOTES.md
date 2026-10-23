# Bedlam 2: Absolute Bedlam PS5 v1.0.0-rc.1

The first 1.0 release candidate is available for PS5 testing. The app's PS5
`contentVersion` is `01.000.000`.

## What's included

The release provides:

- `PPSA94003.zip`: the native PS5 application package.
- `bedlam2-ps5-v1.0.0-rc.1-source.tar.gz`: the source snapshot, without original game data.
- `SHA256SUMS`: SHA-256 checksums for both archives.

Original DOS game data is not included; provide your own files from a copy you
are legally entitled to use.

## Features

- Native PS5 port using the game's engine, with DualSense controls.
- Twin-stick play: left stick moves the active robot; right stick aims.
- D-pad main-menu navigation and squad robot selection during gameplay.
- L1/R1 weapon cycling; D-pad up/down zoom; Triangle toggles the map; Square centers the view.
- R3 cycles HUD scale through 100%, 150%, and 200%.
- The original game crosshair is enlarged, shadowed, and visible in gameplay and weapon selection.
- Pause and exit-level notices with a drop shadow, plus an exit confirmation.
- The active PS5 profile name can fill the game's default player-name field.

## Manual installation

1. Download `PPSA94003.zip` from the [release page](https://github.com/ChaDea1/Bedlam2-PS5/releases/tag/01.000.000-rc.1) and extract it on your computer.
2. Connect to your PS5 homebrew FTP server and copy the extracted `PPSA94003/` folder to `/data/homebrew/`.
3. Open [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) and register `/data/homebrew/PPSA94003/` as an app folder.
4. Use FTP to copy the contents of your own Bedlam 2 game-data folder to `/data/homebrew/PPSA94003/assets/bed2/`. Create the directory if needed, and put the files directly there rather than inside another `bed2/` folder.
5. Launch Bedlam 2 from ShadowMountPlus. The mounted app reads the game data from `/app0/assets/bed2`.

Use game files from a copy you are legally entitled to use. The repository and
release package do not provide them. The [README](https://github.com/ChaDea1/Bedlam2-PS5#readme)
has additional details about the game-data path and controls. After catalog
acceptance, the app can also be installed through ProsperoStore.

## DualSense controls

| Input | Action |
| --- | --- |
| Left stick | Move the active robot |
| Right stick | Aim the weapon and move the crosshair; position the cursor in weapon selection |
| D-pad left / right | Select the previous / next robot in your squad, when available |
| D-pad up / down | Zoom in / out during gameplay; navigate main-menu options outside gameplay |
| L1 / R1 | Select the previous / next weapon |
| Triangle | Toggle the mini-map and mission-briefing map |
| Square | Center the view on the active robot |
| R3 (press right stick) | Cycle HUD size: 100%, 150%, 200% |
| Touchpad click | Pause; click again to resume |
| Options | Send Escape; during gameplay, open the exit-level confirmation |
| Cross | Confirm/select the current option |
| Circle | Secondary mouse button; cancels the exit prompt |
| R2 | Primary fire during gameplay |

In the exit prompt, use the D-pad to choose Yes or No, Cross to confirm, or
Circle/Options to choose No. Some inputs only act on screens where the related
game action is available.

## Known behavior

The first visit to the weapon-selection screen can load more slowly than later
visits.
