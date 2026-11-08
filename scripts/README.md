# Dev commands (PowerShell)

`dev.ps1` adds short commands that run builds in the MSYS2 environment, so PowerShell needs no PATH changes. The commands do nothing until you load the script.

## Load it

Once per PowerShell window:

```powershell
. E:\Project\vcv-rack-plugins\scripts\dev.ps1
```

Note the leading `. ` (dot and space). Without it the functions disappear when the script ends.

To load it in every new window, run this once:

```powershell
Add-Content $PROFILE '. E:\Project\vcv-rack-plugins\scripts\dev.ps1'
```

## Commands

They work from any folder.

| Command | What it does |
|---|---|
| `wa-build` | Build the waveArtifact plugin (`plugin.dll`) |
| `wa-install-public` | Build the MinGW64 version and install it into the **real (downloaded) Rack** (see below) |
| `wa-install` | Install the UCRT build into the Rack user folder. **Do not use for the real Rack, it crashes it** |
| `wa-dist` | Create the `.vcvplugin` package in `waveArtifact/dist` |
| `wa-clean` | Delete the plugin build output |
| `wa-test` | Run the offline analysis tests (no Rack needed) |
| `rack-build` | Build the Rack source in `Rack/` |
| `rack-run` | Launch Rack from source (loads the plugin from `Rack/plugins`) |
| `msys "<cmd>"` | Run any command in MSYS2, e.g. `msys "ls /e/Project"` |
| `mingw` | Open an interactive MinGW 64-bit shell |

Typical loop: `wa-build`, then `rack-run`.

## Install on the real (downloaded) Rack

There are two different Racks on this PC, and they need different builds:

| | Rack from source (`Rack/`) | Real Rack (`F:\Programs\VCV\Rack2Pro`) |
|---|---|---|
| Start it with | `rack-run` | its normal shortcut |
| Plugin build | UCRT64 (`wa-build`) | MinGW64 (`wa-install-public`) |
| Loads the plugin from | `Rack\plugins\waveArtifact` (a link to the repo) | `%LOCALAPPDATA%\Rack2\plugins-win-x64\waveArtifact` |

The real Rack uses the older `msvcrt` runtime. A plugin built the normal way (UCRT) crashes it at startup, which looks like "Rack died at opening". So for the real Rack, always use:

```powershell
# 1. Close the real Rack first (the install refuses while it runs)
# 2. In PowerShell, with dev.ps1 loaded (see "Load it" above):
wa-install-public
```

Or, without dev.ps1, in any PowerShell window:

```powershell
& "C:\msys64\usr\bin\bash.exe" -lc "bash /e/Project/vcv-rack-plugins/scripts/release_public.sh waveArtifact"
```

What it does: copies the plugin to a scratch folder, builds it with MinGW64, checks that the result has no UCRT imports (it refuses to install otherwise), deletes any old `.vcvplugin` package, and copies the plugin to the Rack folder. A successful run prints `built OK (msvcrt, no UCRT imports)` and then `installed: ...`. If it prints `NOT INSTALLED: the public Rack ... is running`, close Rack and run it again (the build is already done).

After installing, start the real Rack normally. Do it again after every code change you want to hear there.

### If the real Rack crashes at startup

1. Close Rack, then run `wa-install-public`. This replaces whatever is installed with the correct build.
2. Check what is installed: a good plugin has no `api-ms-win-crt` imports:
   ```powershell
   & "C:\msys64\mingw64\bin\objdump.exe" -p "$env:LOCALAPPDATA\Rack2\plugins-win-x64\waveArtifact\plugin.dll" | Select-String "api-ms-win-crt|msvcrt.dll"
   ```
   Lines with `api-ms-win-crt` mean the wrong (UCRT) build is installed; a line with `msvcrt.dll` and none with `api-ms-win-crt` is right.
3. Never run `wa-install`, `wa-dist` or `make install` for the real Rack, and don't double-click a `.vcvplugin` made by `wa-dist`: Rack unpacks such packages over the installed folder at startup and brings the crash back. The log (`%LOCALAPPDATA%\Rack2\log.txt`) shows `Extracting package` when that happened.

## Notes

- Do not set `RACK_DIR` globally. The helpers set it only for plugin commands, because a set `RACK_DIR` breaks the Rack source build (`make dep`).
- Bash syntax such as `cd /e/Project/...` or `export X=...` does not work in PowerShell. Use these commands or the `mingw` shell.
- Paths are hard-coded in `dev.ps1`. If you move the repo or the SDK, edit `$script:VcvRoot` and `$script:VcvSdk` at the top.
