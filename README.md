Alpine Faction
============
<img src="https://raw.githubusercontent.com/GooberRF/alpinefaction/refs/heads/master/docs/alpinelogo1.png">

About
-----
Alpine Faction is a patch/modification for the 2001 FPS game Red Faction. While Alpine Faction
is not a source port, its goals and features are similar to what you might expect from one.

Alpine Faction is a fork of rafalh's <a href="https://github.com/rafalh/dashfaction">Dash Faction</a>.

Alpine Faction project goals:
* Fix bugs in the original game
* Resolve security vulnerabilites
* Improve compatibility with modern hardware and operating systems
* Enhance engine performance and graphical quality
* Modernize the game by adding features expected in modern games
* Restore cut functionality from the original game
* Empower players with extensive control over their gameplay experience
* Offer server operators enhanced flexibility to customize their servers as desired
* Equip level and mod designers with a robust set of tools to create awesome stuff

Alpine Faction requires:
* Windows 7 or newer (or you can use Wine)
* Any official distribution of Red Faction (Steam, GoG, retail, etc.)
    * All official localizations are supported (English, French, German)
    * Many other localizations are supported via mods

Key Features
-----
Most important:
* Fixes for critical security vulnerabilities
* Multiplayer level automatic downloader (using API at https://autodl.factionfiles.com)
* Fix for infamous submarine explosion bug (and other FPS-related issues)
* Enemies explode into gibs when killed by explosives
* Autosave at the start of every level
* Support for any resolution and aspect ratio
* Fullscreen, windowed, borderless window modes
* Enhanced mouse input
* Ability to skip cutscenes
* Restored water/lava rising functionality in Geothermal Plant
* Enhanced graphics options such as anti-aliasing and full color range lighting
* High resolution HUD and vector fonts
* Many performance and graphical improvements

Multiplayer:
* Increased (and configurable) tick rate
* "GunGame" game mode
* Competitive match framework including "ready up" system and overtime
* Hitsounds
* Random critical hits (configurable)
* Improved scoreboard
* Voting system for kicking players and changing levels
* First person and free camera spectate
* Cheating prevention
* Many server fixes, performance improvements, and customizable features
* `rf://` protocol handler for joining servers

Level/mod development:
* DDS texture support
* OGG audio support
* Many new event scripting objects for use crafting logic in maps
* Support for dynamic lights in maps
* Support for using dynamic lights, particle emitters, and push regions with movers
* Removed legacy PS2 compatibility measures that resulted in decreased map performance
* Access for many hardcoded settings to be customized in mods
* Support for custom HUDs and translation packs as clientside mods
* Ability to use custom meshes in multiplayer
* Enhanced trigger options for multiplayer
* Full color range lightmaps (removed lightmap clamping)
* Support for custom BIK videos in mods
* Many engine geometry and object limits raised or removed
* Many level editor bug fixes and performance improvements

See the [CHANGELOG file](docs/CHANGELOG.md) for a detailed list of all features.

Compatibility
-------------
Alpine Faction is directly compatible only with Red Faction 1.20 North America (retail).

If you have another official distribution, Alpine Faction will attempt to apply the patches required:
* For 1.00 or 1.10 NA, the installer will apply the 1.20 official patch.
* For other retail localizations (French/German), the installer will patch your game executable as needed and maintain your localization.
* For digital distributions (Steam, GoG, etc.), the installer will patch your game executable as needed.

If you are somehow using an unsupported game executable, the installer and launcher will prompt you to obtain the correct one when you try to launch the game.

Usage
-----
1. Either unpack the zip release or run the installer. Note that Alpine Faction does NOT have to be installed to your Red Faction folder.

2. Run `AlpineFactionLauncher.exe`.

3. Adjust options as desired.

4. Click the "Play Game" button to start playing.

Advanced usage
--------------
This readme file is a work in progress. Alpine Faction has many new client and server commands that are not yet documented.
If you want to try them out now, use `. <FRAGMENT>` to search for available commands.

To use Red Faction command line options with Alpine Faction, use them with `AlpineFactionLauncher.exe`. They will be forwarded to the Red Faction process.

Alpine Faction additionally supports the following command line options:

* `-game` - launch the game (do not display the launcher)
* `-editor` - launch the level editor (do not display the launcher)
* `-win32-console` - if launching a dedicated server, use a native Win32 console window
* `-exe-path` - use the specified path to RF.exe or RED.exe instead of the `Game Executable` field in options

Problems
--------
If your antivirus software detects Alpine Faction as malicious, you may need to explicitly allow it to run.
While Alpine Faction is safe to use, many antivirus vendors wrongly flag new programs as malicious/suspicious.
If you do not trust officially provided Alpine Faction distributions, you can review the code and compile it yourself.

If you run into any problems or have questions, please ask in the Faction Files Discord server: https://discord.gg/factionfiles

Building
--------

See [docs/BUILDING.md](docs/BUILDING.md) for information about building Alpine Faction from source.

License
-------
Alpine Faction's source code is licensed under Mozilla Public License 2.0. See [LICENSE.txt](LICENSE.txt).

Alpine Faction includes contributions from several authors. See [resources/licensing-info.txt](resources/licensing-info.txt) for more information

Alpine Faction is a fork of Dash Faction, which is developed by rafalh. Licensing information for Dash Faction is available here:
https://github.com/rafalh/dashfaction?tab=readme-ov-file#license
