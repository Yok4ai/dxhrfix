@echo off
rem dxhrfix: put the original DFEngine.dll back.
rem
rem Usage: revert.bat ["C:\path\to\Deus Ex Human Revolution Director's Cut"]
rem   With no argument the game folder is the folder this script is in, or its
rem   parent folder (same rule as install.bat).
rem
rem Copies DFEngine_orig.dll over DFEngine.dll and changes nothing else.
rem DFEngine_orig.dll, dxhrfix.ini, dxhrfix_syms.tsv and the logs stay in the
rem game folder; the stock DLL ignores them. Steam, Properties, Installed
rem Files, Verify integrity also restores the stock DLL.
setlocal EnableExtensions
set "PAUSE_AT_END="
if "%~1"=="" set "PAUSE_AT_END=1"

for %%I in ("%~dp0.") do set "HERE=%%~fI"

set "GAME="
if not "%~1"=="" for %%I in ("%~1") do set "GAME=%%~fI"
if not defined GAME if exist "%HERE%\DXHRDC.exe" set "GAME=%HERE%"
if not defined GAME if exist "%HERE%\..\DXHRDC.exe" for %%I in ("%HERE%\..") do set "GAME=%%~fI"
set "MSG=DXHRDC.exe not found next to this script or in its parent folder. Usage: revert.bat "C:\path\to\game folder""
if not defined GAME goto fail
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"

echo dxhrfix revert
echo game folder: "%GAME%"
set "MSG=DXHRDC.exe not found in the game folder"
if not exist "%GAME%\DXHRDC.exe" goto fail
set "MSG=DFEngine_orig.dll not found, nothing to revert to. Use Steam Verify integrity instead."
if not exist "%GAME%\DFEngine_orig.dll" goto fail
set "MSG=the game is running, close it first"
tasklist /FI "IMAGENAME eq DXHRDC.exe" 2>nul | find /I "DXHRDC.exe" >nul
if not errorlevel 1 goto fail

echo copying DFEngine_orig.dll over DFEngine.dll
set "MSG=copy failed"
copy /Y "%GAME%\DFEngine_orig.dll" "%GAME%\DFEngine.dll" >nul
if errorlevel 1 goto fail
set "MSG=copy does not verify"
fc /B "%GAME%\DFEngine_orig.dll" "%GAME%\DFEngine.dll" >nul
if errorlevel 1 goto fail
echo done. The game now runs with the original DFEngine.dll.
if defined PAUSE_AT_END pause
exit /b 0

:fail
echo ERROR: %MSG%
if defined PAUSE_AT_END pause
exit /b 1
