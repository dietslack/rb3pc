# rb3pc

A PC port of Rock Band 3.

The Xbox 360 version of the game is converted ahead of time into a regular
PC program, then fixed up and extended so it plays well on a computer: windowed or
fullscreen play.

Made by slack. I only have a Linux system, so pretty much everything so far
expects Linux. Windows compatibility is probably low for now, but it will be
worked on.

**You need your own copy of Rock Band 3 for Xbox 360.** This repository
contains no game files, and the build only works with files you provide.

## Known issues

Windows builds haven't been tried yet.

## Credits

| Project | Used for |
|---------|----------|
| [ReXGlue](https://github.com/rexglue/rexglue-sdk) | The recompiler and Xbox 360 runtime this port is built on |
| [Xenia](https://github.com/xenia-project/xenia) | The Xbox 360 emulator that ReXGlue's runtime and graphics come from |
| [Rock Band 3 decompilation](https://github.com/DarkRTA/rb3) | Reference for the game's internals: class layouts, function names, file formats |
