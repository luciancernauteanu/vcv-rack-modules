#!/bin/bash
# Build the plugin for the PUBLIC VCV Rack (msvcrt, MSYS2 MINGW64) in a scratch copy and install it.
# A UCRT64 build crashes the public Rack (see SKILL.md), so never install the normal build there.
# Usage: bash release_public.sh [plugin-dir-name]   (default waveArtifact)   or, from PowerShell: wa-install-public
# Refuses to install while the public Rack (F:\Programs\VCV\Rack2Pro) is running; the build still
# happens, so the install is a quick re-run after closing it.
PLUGIN=${1:-waveArtifact}
# A login shell started from PowerShell has a short PATH (no tasklist, no powershell) and may not pass
# LOCALAPPDATA on: add the Windows folders and ask Windows for it.
export PATH="$PATH:/c/Windows/System32:/c/Windows/System32/WindowsPowerShell/v1.0"
[ -n "$LOCALAPPDATA" ] || LOCALAPPDATA=$(powershell.exe -NoProfile -Command '[Environment]::GetFolderPath("LocalApplicationData")' | tr -d '\r')
ROOT=/e/Project/vcv-rack-plugins
SDK=$ROOT/Rack-SDK-2.6.6-win-x64/Rack-SDK
SCRATCH=$(cygpath -u "$LOCALAPPDATA")/Temp/$PLUGIN-mingw64   # same real path in every MSYS2 shell, unlike /tmp
DEST="$LOCALAPPDATA/Rack2/plugins-win-x64"
OBJDUMP=/c/msys64/mingw64/bin/objdump.exe

rm -rf "$SCRATCH" && mkdir -p "$SCRATCH" || exit 1
(cd "$ROOT/$PLUGIN" && tar --exclude=./build --exclude=./dist --exclude=./plugin.dll --exclude=./tests -cf - .) | (cd "$SCRATCH" && tar -xf -)
/c/msys64/usr/bin/env MSYSTEM=MINGW64 /c/msys64/usr/bin/bash.exe -lc \
  "export PATH=/mingw64/bin:/usr/bin; cd '$SCRATCH' && RACK_DIR=$SDK make -j4 dist 2>&1 | grep -E ' error |Error ' | head -20"
[ -f "$SCRATCH/dist/$PLUGIN/plugin.dll" ] || { echo "BUILD FAILED"; exit 1; }
ucrt=$($OBJDUMP -p "$SCRATCH/dist/$PLUGIN/plugin.dll" | grep -c "api-ms-win-crt")
[ "$ucrt" = 0 ] || { echo "REFUSING: plugin links UCRT ($ucrt imports); wrong environment"; exit 1; }
echo "built OK (msvcrt, no UCRT imports)"

if tasklist //FI "IMAGENAME eq Rack.exe" //FO CSV //NH | grep -q .; then
  for p in $(powershell -NoProfile -Command "(Get-Process Rack -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like 'F:*' }).Id"); do
    echo "NOT INSTALLED: the public Rack (pid $p) is running. Close it and re-run this script."; exit 2
  done
fi
# Rack unpacks any .vcvplugin it finds at startup OVER the installed folder, so remove stale packages.
rm -f "$DEST/$PLUGIN"-*.vcvplugin
rm -rf "$DEST/$PLUGIN" && cp -r "$SCRATCH/dist/$PLUGIN" "$DEST/$PLUGIN" && echo "installed: $DEST/$PLUGIN"
