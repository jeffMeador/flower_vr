@echo off
rem Flower VR installer: run from the Flower (GOG) game folder, next to Flower.exe.
cd /d "%~dp0"
if not exist Flower.exe (
  echo Put all these files in your Flower game folder, next to Flower.exe, and run this again.
  pause
  exit /b 1
)

rem The mod forwards to the real Direct3D 11: a copy of this PC's own d3d11.dll
rem (never shared - every Windows install makes its own).
copy /y "%WINDIR%\System32\d3d11.dll" d3d11_orig.dll >nul
if errorlevel 1 (
  echo Could not copy %WINDIR%\System32\d3d11.dll.
  pause
  exit /b 1
)

rem Settings: only created if you don't have them yet (keeps your changes).
if not exist vrmod.ini copy vrmod.ini.example vrmod.ini >nul
if not exist vrmod_Flower.cfg copy vrmod_Flower.cfg.example vrmod_Flower.cfg >nul

rem Overrides generated from your copy of the game (skip the level movies).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0vrmod_make_overrides.ps1" -GameDir "%~dp0." -NoCamGrabs

echo.
echo Flower VR installed. Start SteamVR, then launch Flower.exe.
echo Settings: vrmod.ini (see the README). To uninstall, delete d3d11.dll,
echo d3d11_orig.dll, vrmod.ini, vrmod_Flower.cfg and the vrmod_overrides folder.
pause
