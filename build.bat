@echo off
setlocal

rem Game folder to deploy to: set GAMEDIR before running to override.
if not defined GAMEDIR set GAMEDIR=%USERPROFILE%\Desktop\Flower_GOG

call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo Failed to initialize VC environment
  exit /b 1
)

if not exist build mkdir build

cl.exe /nologo /EHsc /MT /std:c++17 /W3 ^
  /I thirdparty\minhook\include /I thirdparty\openxr ^
  src\dllmain.cpp src\proxy_exports.cpp src\hooks.cpp src\capture.cpp src\stereo.cpp src\fakepad.cpp src\xr.cpp src\camfind.cpp src\camoverride.cpp src\shadow.cpp src\fileredirect.cpp src\displaymodes.cpp src\terrain.cpp src\mirror.cpp src\defaults.cpp src\game.cpp src\journeycam.cpp ^
  thirdparty\minhook\src\buffer.c thirdparty\minhook\src\hook.c thirdparty\minhook\src\trampoline.c thirdparty\minhook\src\hde\hde64.c ^
  /LD /Fe:build\d3d11.dll /Fo:build\ ^
  /link /DEF:src\d3d11_proxy.def d3dcompiler.lib dxguid.lib user32.lib dxgi.lib xinput9_1_0.lib advapi32.lib /OUT:build\d3d11.dll

if errorlevel 1 (
  echo Build FAILED
  exit /b 1
)

copy /y "%WINDIR%\System32\d3d11.dll" build\d3d11_orig.dll >nul

echo Build succeeded. Deploying to %GAMEDIR%...
copy /y build\d3d11.dll "%GAMEDIR%\d3d11.dll" >nul
copy /y build\d3d11_orig.dll "%GAMEDIR%\d3d11_orig.dll" >nul
rem The overrides (movie skip, camera flights) are made from Flower's scripts.
if exist "%GAMEDIR%\Flower.exe" powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_overrides.ps1 -GameDir "%GAMEDIR%" -NoCamGrabs

echo Done. Launch Flower.exe in %GAMEDIR%, then check vrmod.log and frame_*.bmp there
