# KH2 Crash Dump

A LuaBackend mod for **Kingdom Hearts II Final Mix PC** (Steam or Epic) that writes a crash
dump and a crash log whenever the game crashes, so the crash can be looked at afterwards.

## Installation

Install `adamboy7/KH2-Crash-Dump` in the OpenKH Mods Manager and build.

Without a mod manager: put `KH2CrashDump.lua` in your KH2 scripts folder and
`io_packages/kh2crashdump.dll` in that folder's `io_packages` folder.

## What you get

Both go in the game's install folder, next to `KINGDOM HEARTS II FINAL MIX.exe`:

- **`KH2Crash.log`**: one record per crash with the kind of crash, the address as an offset
  into the game exe, what address was read or written, the registers, and the game-exe
  offsets found on the stack (mostly the functions that led to the crash).
- **`KH2Crash-<date>-<time>.dmp`**: a Windows minidump. Open it in WinDbg or Visual
  Studio, or read it with Python's `minidump` package. It holds every thread's stack and
  the game's own data sections (KH2 keeps its work memory and heaps there), usually
  100-200 MB. The last 5 are kept; older ones are deleted when a new one is written.

## Settings

At the top of `KH2CrashDump.lua`:

| Setting | Default | Meaning |
|---|---|---|
| `PREFIX` | `"KH2Crash"` | File name start (a relative path is relative to the game folder) |
| `KEEP` | `5` | Dumps to keep; `0` keeps all of them |
| `FULL` | `false` | `true` adds all of the game's memory: gigabytes, and slow to write |

## How it works

`kh2crashdump.dll` is a small Lua module (Lua 5.4.8 built in, as LuaBackend has no
exported Lua API). It hooks the process's unhandled-exception filter, so only real crashes
count: exceptions that the game or another mod catches never make a dump. The filter that
was installed before it is still called afterwards.

The dump is written by a helper thread started when the mod loads. A crashing thread can be
out of stack, and `MiniDumpWriteDump` needs a lot of it: written on the crashing thread,
the file comes out empty (0 bytes), as Panacea's `CrashDump.dmp` does. `MiniDumpWriteDump`
is loaded from `System32\dbgcore.dll` by full path, because the game folder's
`DBGHELP.dll` is Panacea's proxy.

Nothing in it is specific to KH2, so the same DLL works under the other LuaBackend games.

## Building

`native\build.bat <folder with the Lua 5.4.8 sources>` (MSVC x64), then copy
`native\build\kh2crashdump.dll` to `io_packages\`.
