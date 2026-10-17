# TwoClient

A fork of [OneClient](https://github.com/Polyfrost/OneLauncher) by Polyfrost, cut down and sped up.

Everything that makes the game run is upstream's work. What changed is listed below.

## What is different

**Launching does not wait on the network.** Pressing Play used to run a bundle
catalog sync, a bundle update, and a package update check first — up to ninety
seconds of it, repeated every fifteen minutes. All of that moved into the
startup sync, which nothing is waiting on. Play now costs what the game costs.

**Closing the game is a request, not a kill.** The stop button sent SIGKILL, so
the game never saved and every deliberate stop was reported as a crash. It now
asks first and only kills a game that will not go. Minecraft's own shutdown
watchdog — which turns a slow close into a crash report — is recognised for
what it is instead of being repeated as "Game crashed" and followed by an offer
to re-download the game.

**Instances can be created.** Upstream provisions one per version and loader
from its catalog, with no way to make your own. New instances get their own
game folder, so they have their own saves and options rather than sharing the
one directory every instance swaps content in and out of.

**Instances can be renamed, and are shown by name** — on the versions page, the
home cards and their own page. Two instances cannot share a name.

**Mods can be enabled or disabled in bulk**, over whatever the current tab,
search and filters leave on screen. Narrowing a set of a few hundred mods down
is how you find the one making the game slow to start or slow to close.

**Nothing is reported home by default.** Crash reporting ships off. PolyPlus is
gone: it held a websocket open for the whole session to report playtime, and its
game-side half runs an Epic Online Services SDK whose shutdown handler was part
of what the watchdog was waiting on.

**Importing from OneClient**, so moving across does not mean starting empty.

## Building

```
cargo run -p twoclient_app
```

`--launcher-dir <path>` roots the settings file, the database and the
single-instance endpoint somewhere else, which is how to try a build without
touching an installed launcher.

## Licence

GPL-3.0, the same as upstream. See [LICENSE](LICENSE) and
[ATTRIBUTION.md](ATTRIBUTION.md).

Not affiliated with or endorsed by Polyfrost, and not affiliated with Mojang or
Microsoft.
