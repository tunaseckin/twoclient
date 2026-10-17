# TwoClient

A fork of [OneClient](https://github.com/Polyfrost/OneLauncher) by Polyfrost, cut down and sped up.

Everything that makes the game run is upstream's work. Here is what changed.

## Launching

Pressing Play used to run a bundle catalog sync, a bundle update and a package
update check before the game started. Together those could hold the launch
button for about ninety seconds, and the fifteen minute throttle meant most
sessions in an evening paid it. All of it moved into the startup sync, which
nothing is waiting on.

## Closing

The stop button sent SIGKILL, so the game never got to save and every
deliberate stop was reported as a crash. It now asks first and only kills a
game that will not go.

Minecraft has its own shutdown watchdog that turns a slow close into a crash
report. The launcher used to repeat that as "Game crashed" and offer to verify
and re-download the game, which fixes nothing. It now says what happened.

## Instances

Upstream provisions one instance per version and loader from its catalog, with
no way to make your own. You can create them here, rename them, and see them by
name on the versions page and the home cards. Two instances cannot share a
name.

New instances get their own game folder, so they keep their own saves and
options instead of sharing the single directory that every instance swaps
content in and out of on launch and exit.

## Mods

Mods can be switched on or off in bulk, over whatever the current tab, search
and filters leave on screen. Narrowing a set of a few hundred mods is how you
find the one making the game slow to start or slow to close.

## Network

Crash reporting ships off.

PolyPlus is gone. It held a websocket open for the whole session to report
playtime, and its game-side half runs an Epic Online Services SDK whose
shutdown handler was part of what the watchdog was waiting on.

An OneClient install can be imported, so moving across does not mean starting
empty.

## Building

```
cargo run -p twoclient_app
```

`--launcher-dir <path>` roots the settings file, the database and the
single-instance endpoint somewhere else. Use it to try a build without touching
an installed launcher.

## Licence

GPL-3.0, the same as upstream. See [LICENSE](LICENSE) and
[ATTRIBUTION.md](ATTRIBUTION.md).

Not affiliated with or endorsed by Polyfrost, Mojang or Microsoft.
