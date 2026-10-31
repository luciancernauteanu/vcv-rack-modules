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
| `wa-install` | Install it into the Rack user folder |
| `wa-dist` | Create the `.vcvplugin` package in `waveArtifact/dist` |
| `wa-clean` | Delete the plugin build output |
| `wa-test` | Run the offline analysis tests (no Rack needed) |
| `rack-build` | Build the Rack source in `Rack/` |
| `rack-run` | Launch Rack from source (loads the plugin from `Rack/plugins`) |
| `msys "<cmd>"` | Run any command in MSYS2, e.g. `msys "ls /e/Project"` |
| `mingw` | Open an interactive MinGW 64-bit shell |

Typical loop: `wa-build`, then `rack-run`.

## Notes

- Do not set `RACK_DIR` globally. The helpers set it only for plugin commands, because a set `RACK_DIR` breaks the Rack source build (`make dep`).
- Bash syntax such as `cd /e/Project/...` or `export X=...` does not work in PowerShell. Use these commands or the `mingw` shell.
- Paths are hard-coded in `dev.ps1`. If you move the repo or the SDK, edit `$script:VcvRoot` and `$script:VcvSdk` at the top.
