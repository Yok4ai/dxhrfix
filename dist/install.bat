@echo off
rem dxhrfix installer for Deus Ex: Human Revolution Director's Cut (Steam).
rem
rem Usage: install.bat ["C:\path\to\Deus Ex Human Revolution Director's Cut"]
rem   With no argument the game folder is the folder this script is in, or its
rem   parent folder (for example when the release was extracted into a
rem   subfolder of the game folder). The game folder is the one that holds
rem   DXHRDC.exe. Double clicking install.bat works for that layout.
rem
rem What it does: checks DXHRDC.exe against the supported Steam build (asks
rem before going on if it differs), keeps the stock DFEngine.dll as
rem DFEngine_orig.dll, copies the dxhrfix DFEngine.dll in (also updates an
rem older dxhrfix), copies dxhrfix.ini only if there is none yet, and copies
rem dxhrfix_syms.tsv. It never deletes anything. Undo with revert.bat.
setlocal EnableExtensions EnableDelayedExpansion

set "EXE_SHA=8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1"
set "DFE_STOCK_SHA=030324902ef29f04a3c1152fac6f52bd26ba64dbdaa71cf24f6d2f97e102145e"
set "MARKER=dxhrfix proxy"
set "PAUSE_AT_END="
if "%~1"=="" set "PAUSE_AT_END=1"

for %%I in ("%~dp0.") do set "HERE=%%~fI"

set "GAME="
if not "%~1"=="" for %%I in ("%~1") do set "GAME=%%~fI"
if not defined GAME if exist "%HERE%\DXHRDC.exe" set "GAME=%HERE%"
if not defined GAME if exist "%HERE%\..\DXHRDC.exe" for %%I in ("%HERE%\..") do set "GAME=%%~fI"
if not defined GAME (
    set "MSG=DXHRDC.exe not found next to this script or in its parent folder. Usage: install.bat "C:\path\to\game folder""
    goto fail
)
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"

echo dxhrfix installer
echo game folder: "%GAME%"
set "MSG=DXHRDC.exe not found in the game folder"
if not exist "%GAME%\DXHRDC.exe" goto fail
set "MSG=DFEngine.dll not found in the game folder"
if not exist "%GAME%\DFEngine.dll" goto fail
set "MSG=DFEngine.dll (dxhrfix) not found next to this script"
if not exist "%HERE%\DFEngine.dll" goto fail
set "MSG=dxhrfix.ini not found next to this script"
if not exist "%HERE%\dxhrfix.ini" goto fail
set "MSG=DFEngine.dll next to this script is not the dxhrfix DLL"
findstr /M /C:"%MARKER%" "%HERE%\DFEngine.dll" >nul 2>nul
if errorlevel 1 goto fail
if /I "%GAME%"=="%HERE%" (
    set "MSG=the release files are in the game folder itself, so the game's DFEngine.dll was replaced when they were extracted. Run Steam, Properties, Installed Files, Verify integrity to get the stock DFEngine.dll back, then extract the release into its own folder, for example a subfolder of the game folder, and run install.bat from there."
    goto fail
)
set "MSG=the game is running, close it first"
tasklist /FI "IMAGENAME eq DXHRDC.exe" 2>nul | find /I "DXHRDC.exe" >nul
if not errorlevel 1 goto fail

echo step 1: checking DXHRDC.exe
call :sha "%GAME%\DXHRDC.exe"
if errorlevel 1 goto fail
if /I "!H!"=="%EXE_SHA%" (
    echo         DXHRDC.exe is the supported Steam build
    goto step2
)
echo WARNING: DXHRDC.exe sha256 is !H!
echo          dxhrfix supports only the Steam build %EXE_SHA%.
echo          Every patch checks the game's bytes first and switches itself off on a
echo          mismatch, so the game should run stock, but this is untested.
set "ANS="
set /p "ANS=Continue anyway? [y/N] "
set "MSG=cancelled, nothing was changed"
if /I "!ANS!"=="y" goto step2
if /I "!ANS!"=="yes" goto step2
goto fail

:step2
echo step 2: checking DFEngine.dll in the game folder
call :sha "%GAME%\DFEngine.dll"
if errorlevel 1 goto fail
if /I "!H!"=="%DFE_STOCK_SHA%" goto stockdll
findstr /M /C:"%MARKER%" "%GAME%\DFEngine.dll" >nul 2>nul
if not errorlevel 1 goto proxydll
set "MSG=DFEngine.dll in the game folder is neither the stock DLL nor dxhrfix (sha256 !H!). Another mod may use it. Run Steam, Properties, Installed Files, Verify integrity, then run install.bat again."
goto fail

:stockdll
echo         DFEngine.dll is the stock ad SDK DLL
if not exist "%GAME%\DFEngine_orig.dll" goto makeorig
call :sha "%GAME%\DFEngine_orig.dll"
if errorlevel 1 goto fail
set "MSG=DFEngine_orig.dll exists but is not the stock DLL (sha256 !H!). Move it out of the game folder and run again."
if /I not "!H!"=="%DFE_STOCK_SHA%" goto fail
echo         DFEngine_orig.dll already exists and is stock, keeping it
goto step3

:makeorig
echo         keeping the stock DLL as DFEngine_orig.dll
set "MSG=copy failed"
copy /Y "%GAME%\DFEngine.dll" "%GAME%\DFEngine_orig.dll" >nul
if errorlevel 1 goto fail
call :sha "%GAME%\DFEngine_orig.dll"
if errorlevel 1 goto fail
set "MSG=DFEngine_orig.dll copy does not verify"
if /I not "!H!"=="%DFE_STOCK_SHA%" goto fail
goto step3

:proxydll
echo         DFEngine.dll is an earlier dxhrfix, updating it
if not exist "%GAME%\DFEngine_orig.dll" (
    echo WARNING: DFEngine_orig.dll is missing, so revert.bat will not work.
    echo          Steam, Properties, Installed Files, Verify integrity restores the stock DLL.
    goto step3
)
call :sha "%GAME%\DFEngine_orig.dll"
if errorlevel 1 goto fail
if /I not "!H!"=="%DFE_STOCK_SHA%" echo WARNING: DFEngine_orig.dll is not the stock DLL ^(sha256 !H!^)

:step3
echo step 3: copying dxhrfix DFEngine.dll into the game folder
set "MSG=copy failed"
copy /Y "%HERE%\DFEngine.dll" "%GAME%\DFEngine.dll" >nul
if errorlevel 1 goto fail
call :sha "%HERE%\DFEngine.dll"
if errorlevel 1 goto fail
set "WANT=!H!"
call :sha "%GAME%\DFEngine.dll"
if errorlevel 1 goto fail
set "MSG=DFEngine.dll copy does not verify"
if /I not "!H!"=="!WANT!" goto fail

if exist "%GAME%\dxhrfix.ini" (
    echo step 4: dxhrfix.ini already exists in the game folder, keeping your settings
) else (
    echo step 4: copying dxhrfix.ini into the game folder
    set "MSG=copy failed"
    copy "%HERE%\dxhrfix.ini" "%GAME%\dxhrfix.ini" >nul
    if errorlevel 1 goto fail
)

if exist "%HERE%\dxhrfix_syms.tsv" (
    echo step 5: copying dxhrfix_syms.tsv ^(function names for the profiler^) into the game folder
    set "MSG=copy failed"
    copy /Y "%HERE%\dxhrfix_syms.tsv" "%GAME%\dxhrfix_syms.tsv" >nul
    if errorlevel 1 goto fail
) else (
    echo step 5: dxhrfix_syms.tsv not found next to this script, skipped ^(only the profiler uses it^)
)

echo done. Start the game, then check that dxhrfix.log appears in the game folder.
if defined PAUSE_AT_END pause
exit /b 0

:fail
echo ERROR: !MSG!
if defined PAUSE_AT_END pause
exit /b 1

rem :sha FILE sets H to the hex sha256 of FILE (fails if certutil fails).
:sha
set "H="
for /f "usebackq skip=1 delims=" %%a in (`certutil -hashfile "%~1" SHA256 2^>nul`) do (
    if not defined H set "H=%%a"
)
if defined H set "H=!H: =!"
set "MSG=could not hash %~1 with certutil"
if not defined H exit /b 1
if "!H:~63,1!"=="" exit /b 1
if not "!H:~64!"=="" exit /b 1
exit /b 0
