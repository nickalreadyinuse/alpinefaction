Alpine Faction
============

About
-----
Alpine Faction is a patch/modification for the 2001 FPS game Red Faction.
While Alpine Faction is not a source port, its goals and features are similar to what you might expect from one.

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
* Windows 7 or newer (or you can use WINE)
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
* New events for use in maps
* Access for many hardcoded settings to be customized in mods
* Support for custom HUDs and translation packs as clientside mods
* Ability to use custom meshes in multiplayer
* Enhanced trigger options for multiplayer
* Full color range lightmaps
* Support for custom BIK videos in mods
* Many engine geometry and object limits raised or removed
* Many level editor bug fixes and performance improvements

See the [CHANGELOG file](docs/CHANGELOG.md) for a detailed list of all features.

You can also check out [examples of graphical improvements compared to the base game](docs/graphics_comparison).

### Client commands

Name                          | Description
----------------------------- | --------------------------------------------
`. cmd_name_fragment`         | find a console variable or command
`maxfps value`                | set maximal FPS
`hud`                         | show and hides HUD
`bighud`                      | toggle HUD between big and normal size
`spectate [player]`           | start spectate mode (first person or free camera)
`inputmode`                   | switch between default and DirectInput mouse input handling
`ms value`                    | set mouse sensitivity
`vli`                         | toggle volumetric lightining
`fullscreen`                  | enter fullscreen mode
`windowed`                    | enter windowed mode
`antialiasing`                | toggle anti-aliasting
`nearest_texture_filtering`   | toggle nearest neighbor texture filtering
`damage_screen_flash`         | toggle screen flash effect when player is getting hit
`mesh_static_lighting`        | toggle static lighting for clutters and items
`reticle_scale scale`         | set reticle scale
`findlevel rfl_name_fragment` | find level using rfl name fragment
`download_level rfl_name`     | download level from FactionFiles.com
`linear_pitch`                | toggle linear pitch angle
`skip_cutscene_bind control`  | set binding for cutscene skip action
`levelsounds`                 | set volume scale for level ambient sounds
`swap_assault_rifle_controls` | swap primary and alternate fire controls for Assault Rifle weapon
`swap_grenade_controls`       | swap primary and alternate fire controls for Grenades weapon
`show_enemy_bullets`          | toggle visibility of enemy bullets in multiplayer
`kill_messages`               | toggle printing of kill messages in the chatbox and the game console
`mute_all_players`            | toggle processing of chat messages from other players
`mute_player`                 | toggle processing of chat messages from a specific player
`fps_counter`                 | toggle FPS counter
`debug_event_msg`             | toggle tracking of event messages in console

### Server commands

Name                     | Description
------------------------ | --------------------------------------------
`unban_last`             | unban last banned player
`map_ext`                | extend round
`map_rest`               | restart current level
`map_next`               | load next level
`map_rand`               | load random level from rotation
`map_prev`               | load previous level
`kill_limit value`       | set kill limit
`time_limit value`       | set time limit
`geomod_limit value`     | set geomod limit
`capture_limit value`    | set capture limit

Compatibility
-------------
Dash Faction is compatible with Red Faction 1.20 North America (NA).
If your game version is 1.00 or 1.10 you have to update it to 1.20 first.
If your edition is not NA or you are using the Steam/GOG version, you have to replace RF.exe with one from the
1.20 NA version (it can be found on https://www.factionfiles.com). The launcher will ask you to do it if it detects
an unsupported executable. The Dash Faction installer does all required changes to the installation automatically.

Supported operating systems: Windows 7 and newer.

Dash Faction also works on Linux when launched through Wine. The latest Ubuntu LTS and vanilla Wine from the latest stable branch are recommended.

Usage
-----
1. Unpack the Dash Faction files to any folder (there is no requirement to put it in the Red Faction folder).

2. Run `DashFactionLauncher.exe`.

3. When running the launcher for the first time, select the "Options" button and adjust the settings to your preference. Make sure the
   game executable path is valid.

4. Close the options window and click the "Launch Game" button to start Dash Faction.

Advanced usage
--------------
You can provide additional command line arguments to the `DashFactionLauncher.exe` application, and they will be forwarded
to the Red Faction process. For example, to start a dedicated server use the `-dedicated` argument just like in the original
game.

Dash Faction specific arguments:

* `-game` - launch the game immediately without displaying the launcher window
* `-editor` - launch the level editor immediately without displaying the launcher window
* `-win32-console` - use a native Win32 console in the dedicated server mode
* `-exe-path` - override the launched executable file path (RF.exe or RED.exe) - useful for running multiple dedicated servers using separate RF directories

Problems
--------
If your antivirus software detects Dash Faction as a malicious program, add it to a whitelist or try to disable
reputation-based heuristics in the antivirus options. It may help as some antivirus programs flag new files as malicious just because they are not widely used.
If you do not trust the author of Dash Faction, you can review its code and compile it yourself - keep in mind it is
open-source software.

During video capture in OBS, please disable MSAA in Options - they do not perform well together.

If you experience any other problems, you can ask for help in the Faction Files Discord server at https://discord.gg/factionfiles.

Additional server configuration
-------------------------------
Dedicated server specific settings are configured in the `dedicated_server.txt` file.
Dash Faction specific configuration must be placed below the level list (`$Level` keys) and must appear in the order
provided in this description.

Configuration example:

    //
    // Dash Faction specific configuration
    //
    // Enable vote kick
    $DF Vote Kick: true
        // Vote specific options (all vote types have the same options)
        // Vote time limit in seconds (default: 60)
        +Time Limit: 60
    // Enable vote level
    $DF Vote Level: true
    // Enable vote extend
    $DF Vote Extend: true
    // Enable vote restart
    $DF Vote Restart: true
    // Enable vote next
    $DF Vote Next: true
    // Enable vote random
    $DF Vote Random: true
    // Enable vote previous
    $DF Vote Previous: true
    // Determine whether players are granted some duration of invulnerability after spawning (stock RF is true)
    $DF Spawn Protection Enabled: true
        // Duration of the invulnerability in ms (stock RF is 1500)
        +Duration: 1500
        // Enable to use an Invulnerability powerup for the spawn protection (intended for run servers)
        +Use Powerup: false
    // Adjust setting related to player respawn logic (defaults match stock game)
    $DF Player Respawn Logic:
        // In team gamemodes (CTF/TeamDM), only spawn players at points associated with their team
        +Respect Team Spawns: true
        // Players are more likely to spawn at spawn points further away from other players
        +Prefer Avoid Players: true
        // Avoid spawning players at the same spawn point twice in a row
        +Always Avoid Last: false
        // Always spawn players at the furthest spawn point from other players (removes RNG)
        +Always Use Furthest: false
        // Ignore teammates when calculating the distance from spawn points to other players
        +Only Avoid Enemies: false
        // Create an additional respawn point at each "Medical Kit" item, if the level has less than 9 Multiplayer Respawn Points
        // Specify "0" to always create them regardless of how many Multiplayer Respawn Points are in the level
        +Use Item As Spawn Point: "Medical Kit" 9
    // Duration of player invulnerability after respawn in ms (default is the same as in stock RF - 1500)
    $DF Spawn Protection Duration: 1500
    // Initial player life (health) after spawn
    $DF Spawn Health: 100
    // Initial player armor after spawn
    $DF Spawn Armor: 0
    // Time before a dropped CTF flag will return to base in ms (default is same as stock RF - 25000)
    $DF CTF Flag Return Time: 25000
    // Enable hit-sounds
    $DF Hitsounds: true
        // Sound used for hit notification
        +Sound ID: 29
        // Max sound packets per second - keep it low to save bandwidth
        +Rate Limit: 10
    // Enable critical hits
    $DF Critical Hits: false
        // Sound used for hit notification
        +Sound ID: 35
        // Max sound packets per second - keep it low to save bandwidth
        +Rate Limit: 10
        // Duration of damage amp reward on a critical hit
        +Reward Duration: 1500
        // Percentage chance of a critical hit
        +Base Chance Percent: 0.1
        // Enable dynamic chance bonus based on damage dealt in current life
        +Use Dynamic Chance Bonus: true
        // Amount of damage to deal in current life for the max dynamic chance bonus (+ 0.1)
        +Dynamic Chance Damage Ceiling: 1200
    // If Weapon Stay is on, set some specific weapon pickups to be exempt (act as if it were not on) for balancing
    $DF Weapon Stay Exemptions: false
        +Flamethrower: false
        +Control Baton: false
        +Riot Shield: false
        +Pistol: false
        +Shotgun: false
        +Submachine Gun: false
        +Sniper Rifle: false
        +Assault Rifle: false
        +Heavy Machine Gun: false
        +Precision Rifle: false
        +Rail Driver: false
        +Rocket Launcher: false
        +Grenade: false
        +Remote Charges: false
    // Replace all "Shotgun" items with "rail gun" items when loading RFLs
    $DF Item Replacement: "Shotgun" "rail gun"
    // If enabled players are given full ammo when picking up weapon items, can be useful with the Weapons Stay standard option
    $DF Weapon Items Give Full Ammo: false
    // Replace default player weapon class
    $DF Default Player Weapon: "rail_gun"
        // Ammo given on respawn (by default 3 * clip size)
        +Initial Ammo: 1000
    // Anti-cheat level determines how many checks the player must pass to be allowed to spawn. Higher levels include
    // checks from lower levels. Default level is 0. Supported levels:
    // 0 - everyone can play
    // 1 - player must use non-custom build of Dash Faction 1.7+ or Pure Faction (unpatched clients are disallowed)
    // 2 - essential game parameters must match (blue P symbol in Pure Faction)
    // 3 - client-side mods are disallowed (gold P symbol in Pure Faction)
    //$DF Anticheat Level: 0
    // Set click limit for semi-automatic weapons (pistol and precision rifle). Bullets fired quicker than this rate will be ignored. Can be used to lessen impact of autoclicker cheats.
    $DF Semi Auto Click Rate Limit: 20.0
    // If true and server is using a mod (-mod command line argument) then client is required to use the same mod
    // Can be disabled to allow publicly available modded servers
    $DF Require Client Mod: true
    // Multiplied with damage when shooting at players. Set to 0 if you want invulnerable players e.g. in run-map server
    $DF Player Damage Modifier: 1.0
    // Enable '/save' and '/load' chat commands (works for all clients) and quick save/load controls handling (works for Dash Faction 1.5.0+ clients). Option designed with run-maps in mind.
    $DF Saving Enabled: false
    // Enable Universal Plug-and-Play (enabled by default)
    $DF UPnP Enabled: true
    // Force all players to use the same character (check pc_multi.tbl for valid names)
    $DF Force Player Character: "enviro_parker"
    // Maximal horizontal FOV that clients can use for level rendering (unlimited by default)
    //$DF Max FOV: 125
    // Shuffle the list of levels when the server starts and at the end of each full rotation
    $DF Dynamic Rotation: false
    // Allow clients to use `mesh_fullbright`
    $DF Allow Fullbright Meshes: false
    // Allow clients to use `lightmaps_only`
    $DF Allow Lightmaps Only Mode: false
    // Allow clients to use `disable_screenshake`
    $DF Allow Disable Screenshake: false
    // If enabled a private message with player statistics is sent after each round.
    //$DF Send Player Stats Message: true
    // Send a chat message to players when they join the server ($PLAYER is replaced by player name)
    //$DF Welcome Message: "Hello $PLAYER!"
    // Reward a player for a successful kill
    $DF Kill Reward:
        // Increase player health or armor if health is full (armor delta is halved)
        +Effective Health: 0
        // Increase player health
        +Health: 0
        // Increase player armor
        +Armor: 0
        // Limit health reward to 200 instead of 100
        +Health Is Super:
        // Limit armor reward to 200 instead of 100
        +Armor Is Super:
    // Enable for round to go to overtime when the time limit is up but score is tied
    $DF Overtime Enabled: false
        // Overtime ends when tie is broken or after this duration (whichever comes first)
        +Duration: 5


Building
--------

Information about building Dash Faction from source code can be found in [docs/BUILDING.md](docs/BUILDING.md).

License
-------
Most of the Dash Faction source code is licensed under Mozilla Public License 2.0. It is available in the GitHub repository.
See [LICENSE.txt](LICENSE.txt).

Only the Pure Faction anti-cheat support code is not open-sourced, because it would make PF anti-cheat features basically useless.
It consists of a few files in the *game_patch/purefaction* directory. It is linked statically during a release process
of Dash Faction by the owner of the project.

Dash Faction uses some open-source libraries. Their licenses can be found in the *licenses* directory.
