@echo off
rem Install the dxhrfix DFEngine.dll proxy into the game folder.
rem Usage: install.bat "C:\path\to\Deus Ex Human Revolution Director's Cut"
rem        (no argument = current folder). Never deletes anything.
setlocal EnableExtensions EnableDelayedExpansion

set "EXE_STOCK=8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1"
set "DFE_STOCK=030324902ef29f04a3c1152fac6f52bd26ba64dbdaa71cf24f6d2f97e102145e"
set "HERE=%~dp0"
if "%~1"=="" (set "GAME=%CD%") else (set "GAME=%~1")

echo game folder: %GAME%
if not exist "%GAME%\DXHRDC.exe" (echo ERROR: DXHRDC.exe not found in the game folder & exit /b 1)
if not exist "%HERE%DFEngine.dll" (echo ERROR: proxy DFEngine.dll not found next to this script & exit /b 1)
if not exist "%HERE%dxhrfix.ini" (echo ERROR: dxhrfix.ini not found next to this script & exit /b 1)
tasklist /FI "IMAGENAME eq DXHRDC.exe" 2>nul | find /I "DXHRDC.exe" >nul && (echo ERROR: the game is running, close it first & exit /b 1)

echo step 1: using the built-in stock hashes
echo step 2: checking DXHRDC.exe
call :sha "%GAME%\DXHRDC.exe" || exit /b 1
if /I not "!H!"=="%EXE_STOCK%" (echo ERROR: DXHRDC.exe sha256 !H! is not the stock exe, refusing & exit /b 1)
echo         DXHRDC.exe is stock

if exist "%GAME%\DFEngine_orig.dll" (
    call :sha "%GAME%\DFEngine_orig.dll" || exit /b 1
    echo step 3: DFEngine_orig.dll already exists ^(sha256 !H!^), keeping it
    if /I not "!H!"=="%DFE_STOCK%" echo         WARNING: DFEngine_orig.dll is not the stock DFEngine.dll
) else (
    call :sha "%GAME%\DFEngine.dll" || exit /b 1
    if /I not "!H!"=="%DFE_STOCK%" (echo ERROR: game DFEngine.dll sha256 !H! is not stock and no DFEngine_orig.dll exists, refusing & exit /b 1)
    echo step 3: copying stock DFEngine.dll to DFEngine_orig.dll
    copy /Y "%GAME%\DFEngine.dll" "%GAME%\DFEngine_orig.dll" >nul || (echo ERROR: copy failed & exit /b 1)
)

echo step 4: copying proxy DFEngine.dll into the game folder
copy /Y "%HERE%DFEngine.dll" "%GAME%\DFEngine.dll" >nul || (echo ERROR: copy failed & exit /b 1)
if exist "%GAME%\dxhrfix.ini" (
    echo step 5: dxhrfix.ini already exists in the game folder, keeping it
) else (
    echo step 5: copying dxhrfix.ini into the game folder
    copy /Y "%HERE%dxhrfix.ini" "%GAME%\dxhrfix.ini" >nul || (echo ERROR: copy failed & exit /b 1)
)
echo done. Start the game, then check "%GAME%\dxhrfix.log"
exit /b 0

:sha
set "H="
for /f "skip=1 delims=" %%a in ('certutil -hashfile "%~1" SHA256') do (
    if not defined H set "H=%%a"
)
if not defined H (echo ERROR: certutil could not hash %~1 & exit /b 1)
set "H=!H: =!"
exit /b 0
