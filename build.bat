@echo off
setlocal

call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo Failed to initialize VC environment
  exit /b 1
)

if not exist build mkdir build

cl.exe /nologo /EHsc /MT /std:c++17 /W3 ^
  src\dllmain.cpp src\proxy_exports.cpp src\hooks.cpp ^
  /LD /Fe:build\d3d11.dll /Fo:build\ ^
  /link /DEF:src\d3d11_proxy.def /OUT:build\d3d11.dll

if errorlevel 1 (
  echo Build FAILED
  exit /b 1
)

copy /y "%WINDIR%\System32\d3d11.dll" build\d3d11_orig.dll >nul

echo Build succeeded. Deploying to ..\ (game folder)...
copy /y build\d3d11.dll ..\d3d11.dll >nul
copy /y build\d3d11_orig.dll ..\d3d11_orig.dll >nul

echo Done. Launch Flower.exe, then check ..\vrmod.log and ..\frame_*.bmp
