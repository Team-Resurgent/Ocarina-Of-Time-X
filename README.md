# Ocarina of Time for the original Xbox

A native original-Xbox port of *The Legend of Zelda: Ocarina of Time*, built from
[Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) 9.3.1 and its engine,
[libultraship](https://github.com/Kenix3/libultraship). The game is Ship of Harkinian's,
compiled for the Xbox's Pentium III; what it runs on is in `xbox/` and libultraship's Xbox
backends.

**No game data is included.** You need your own Ocarina of Time ROM. Everything taken from
it is made on your own machine.

| ROM | SHA-1 (`.z64`) | |
| --- | --- | --- |
| USA (Rev 2), "1.2" | `41b3bdc48d98c48529219919015a1af22f5057c2` | tested |

## Features

* The full game, with Ship of Harkinian's fixes and enhancements
* 720p, 480p or 480i, following the dashboard's video settings, in 16:9
* An in-game menu on **Back**: fully remappable controls, options and cheats
* Options for skipping cutscenes, faster text and chests, D-pad item equips, enemy health
  bars, sound volumes and many more of Ship of Harkinian's enhancements
* Each save file is its own Xbox save game, managed from the dashboard
* Rumble, with full, medium and low strengths

## Requirements

* Windows 10 or 11
* the RXDK toolchain: in Visual Studio, **RXDK > RXDK Window > Install Prerequisites**
  (or RXDK's own installer), in `C:\ProgramData\RXDK`
* [Python](https://www.python.org/downloads/) 3.8 or later
* [Git](https://git-scm.com/download/win)
* [Visual Studio](https://visualstudio.microsoft.com/) 2022 or later with **Desktop
  development with C++** (it includes **C++ CMake tools for Windows**, which build Ship of
  Harkinian's archive tools; the free Build Tools edition is enough)
* an internet connection the first time you build: Ship of Harkinian's asset definitions,
  [Torch](https://github.com/HarbourMasters/Torch) (its asset extractor) and the libraries
  the game is built with are downloaded, each at one exact commit, so nothing that changes
  upstream changes this build

`python setup.py --check` lists what is there and what is missing.

## Building

1. Download this repository (**Code > Download ZIP**, or `git clone`).
2. Put your ROM in its folder, named `baserom.z64` (a byte-swapped `.v64` or `.n64` is put
   in order for you).
3. Double-click **`build.bat`**, or in a console:

```bash
python setup.py
```

The result is **`out\Release\XISO\Ocarina-Of-Time-X.iso`**. The first build takes a while
(the archive tools, the extraction and the game are all built); later ones only redo what
changed.

The build runs, in order:

1. checks the ROM (SHA-1) against the versions Ship of Harkinian 9.3.1 extracts and puts it
   in `.z64` order (`build/rom/`);
2. fetches Ship of Harkinian 9.3.1's asset definitions and its own extra files
   (`build/upstream/Shipwright`, only those folders), Torch (`build/upstream/torch`) and the
   libraries (`deps/`);
3. builds Ship of Harkinian's two archive tools with Visual Studio (`tools/extract`, into
   `build/extract/`);
4. extracts the game from your ROM into `data/oot.o2r`, and packs Ship of Harkinian's own
   extras into `data/soh.o2r`;
5. builds the game with the RXDK (`tools/build.py`), makes the dashboard pictures from the
   ROM's Ocarina icon, and packs the disc.

```bash
python setup.py --check         # what is present and what is missing
python setup.py --tester        # with the tester tools: out\Tester\ (below)
python setup.py --skip-build    # everything but the game's compile
```

`--tester` adds what a test run needs on a retail console with nothing listening: a log
file and a black box on the hard disk that survive a crash or a power-off, a report of the
last run's end at the next start, and an on-screen overlay of the frame rate and memory
(the log with **Logging**, the overlay with **Test overlay**, both on the port menu's Debug
page). `tools\pull_logs.ps1` copies the files
off the console over its FTP server.

## Running

* **Disc image:** boot `out\Release\XISO\Ocarina-Of-Time-X.iso` on a modded console or in
  [xemu](https://xemu.app/).
* **Hard disk:** copy the folder `out\Release\Build\Ocarina-Of-Time-X\` (`default.xbe`, the
  two `.o2r` archives and the menu font) to your Xbox and launch `default.xbe`.

## What is in this repository

* `Shipwright/` -- Ship of Harkinian 9.3.1's game code (`soh/src`, `soh/soh`, `soh/include`,
  the asset name headers) and its two archive tools' sources, with the Xbox port's changes;
  not its asset definitions or extra files, which `setup.py` fetches
* `libultraship/` -- libultraship at the commit Ship of Harkinian 9.3.1 uses, with its Xbox
  port: the Direct3D 8 renderer, DirectSound, the Xbox controllers and file system
* `xbox/` -- the Xbox side: `main`, the port's menu, saves, the tester tools
* `tools/` -- `build.py` (the game's build with the RXDK's clang), `fetch_deps.py`,
  `make_icons.py`, `extract/` (the archive tools' CMake project), `pull_logs.ps1`
* `setup.py`, `build.bat` -- from the ROM to the disc

## Credits

* [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) by HarbourMasters, from
  the [Ocarina of Time decompilation](https://github.com/zeldaret/oot) by ZeldaRET
* [libultraship](https://github.com/Kenix3/libultraship) and
  [Torch](https://github.com/HarbourMasters/Torch)
* the RXDK, by Team Resurgent
