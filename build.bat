@echo off
rem build.bat - build kill-process-type.exe (MinGW-w64 gcc + windres)
rem NOTE: keep this file pure ASCII + CRLF, Chinese comments break under GBK codepage
setlocal
cd /d "%~dp0"

rem prefer Strawberry windres 2.42 (newer, more reliable manifest embedding)
set WINDRES=C:\Strawberry\c\bin\windres.exe
if not exist "%WINDRES%" set WINDRES=windres

if not exist build mkdir build

"%WINDRES%" res\app.rc -O coff -o build\app_res.o
if errorlevel 1 goto :err

gcc -municode -mwindows -O2 -Wall -Wextra -static -o build\kill-process-type.exe src\main.c src\gui.c src\views.c src\actions.c src\ai.c src\cli.c src\richtext.c src\config.c src\theme.c src\settings.c src\tray.c src\process.c src\net.c src\startup.c build\app_res.o -lcomctl32 -lpsapi -liphlpapi
if errorlevel 1 goto :err

echo Build OK: build\kill-process-type.exe
pause
exit /b 0

:err
echo Build FAILED.
pause
exit /b 1
