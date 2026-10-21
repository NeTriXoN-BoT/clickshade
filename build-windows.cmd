@echo off
setlocal
cd /d "%~dp0"
cmake -S . -B build-msvc -A x64
if errorlevel 1 exit /b 1
cmake --build build-msvc --config Release
if errorlevel 1 exit /b 1
echo.
echo Built: build-msvc\Release\ClickShade.exe
