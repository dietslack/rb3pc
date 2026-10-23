# rb3pc

A PC port of Rock Band 3.

The Xbox 360 version of the game is converted ahead of time into a regular
PC program, then fixed up and extended so it plays well on a computer: any
window size, mouse support, microphones, MIDI drum kits, and custom songs.
It can also run the popular [Rock Band 3 Deluxe](https://github.com/hmxmilohax/rock-band-3-deluxe)
mod.

Made by slack. I only have a Linux system, so pretty much everything so far
expects Linux. Windows compatibility is probably low for now, but it will be
worked on.

**You need your own copy of Rock Band 3 for Xbox 360.** This repository
contains no game files, and the build only works with files you provide.

## Known issues

Keys and pro guitar aren't supported yet. Clicking inside another player's
menu acts as player 1. Typing on a keyboard doesn't work in text boxes yet.
Windows builds haven't been tried yet. Not every Rock Band 3 Deluxe feature
has been checked.

## Sources and credits

| Project | Used for |
|---------|----------|
| [ReXGlue](https://github.com/rexglue/rexglue-sdk) | The recompiler and Xbox 360 runtime this port is built on |
| [Xenia](https://github.com/xenia-project/xenia) | The Xbox 360 emulator that ReXGlue's runtime and graphics come from |
| [Rock Band 3 decompilation](https://github.com/DarkRTA/rb3) | Reference for the game's internals: class layouts, function names, file formats |
| [Rock Band 3 Deluxe](https://github.com/hmxmilohax/rock-band-3-deluxe) | The mod rb3pc can run; its executable is recompiled as `rb3pc-dx` |
| [RB3Enhanced](https://github.com/RBEnhanced/RB3Enhanced) | Reference for the script functions Deluxe expects (rb3pc has its own versions) and their Xbox 360 addresses |
| [onyx](https://github.com/mtolly/onyx) (moggcrypt) | The decryption keys for the game's song audio files |
| [Se7enSins forum post](https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/) | How the Xbox 360's on-screen keyboard function is called |
| [SDL](https://github.com/libsdl-org/SDL) | Windows, controllers, sound and microphones (through ReXGlue) |
| [Dear ImGui](https://github.com/ocornut/imgui) | The settings screen and settings windows (through ReXGlue) |
| [zlib](https://zlib.net) | Unpacking the game's compressed files and the Deluxe download, checksums, screenshots |
| [ALSA](https://www.alsa-project.org) | MIDI input on Linux |

Rock Band is a trademark of Harmonix Music Systems. This project isn't
affiliated with or endorsed by Harmonix.

## License

MIT, see [LICENSE](LICENSE). This covers the code in this repository only,
not the game or anything generated from it.
