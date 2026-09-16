@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo Failed to initialize VC environment
  exit /b 1
)

if not exist build mkdir build

cl.exe /nologo /EHsc /MT /std:c++17 reflect_shaders.cpp ^
  /Fe:build\reflect_shaders.exe /Fo:build\ ^
  /link d3dcompiler.lib dxguid.lib /OUT:build\reflect_shaders.exe

if errorlevel 1 (
  echo Build FAILED
  exit /b 1
)

rem D3DReflect needs the real D3DCOMPILER_47.dll at runtime; the game folder already has one.
copy /y "..\..\D3DCOMPILER_47.dll" build\D3DCOMPILER_47.dll >nul

echo Build succeeded: build\reflect_shaders.exe
