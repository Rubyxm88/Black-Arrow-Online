@echo off
cd /d "%~dp0"
echo This removes the console set up in this folder: its identity, saved games
echo and Xbox Live sign-in. Your game disc images are not touched, and neither
echo are any files outside this folder.
echo.
echo Close Black Arrow Online first.
echo.
set /p ANSWER=Type RESET and press Enter to continue:
if /i not "%ANSWER%"=="RESET" (
    echo Nothing was changed.
    pause
    exit /b
)
if exist eeprom.bin del eeprom.bin
if exist xbox_hdd.qcow2 del xbox_hdd.qcow2
echo Done. Start Black Arrow Online to choose a clean start or your own files.
pause
