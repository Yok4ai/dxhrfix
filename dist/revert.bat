@echo off
rem Put the original DFEngine.dll back: copies DFEngine_orig.dll over
rem DFEngine.dll. Never deletes anything; DFEngine_orig.dll, dxhrfix.ini and
rem dxhrfix.log stay (harmless with the stock DLL).
rem Usage: revert.bat "C:\path\to\game folder" (no argument = current folder)
setlocal EnableExtensions
if "%~1"=="" (set "GAME=%CD%") else (set "GAME=%~1")
echo game folder: %GAME%
if not exist "%GAME%\DFEngine_orig.dll" (echo ERROR: DFEngine_orig.dll not found, nothing to revert to & exit /b 1)
tasklist /FI "IMAGENAME eq DXHRDC.exe" 2>nul | find /I "DXHRDC.exe" >nul && (echo ERROR: the game is running, close it first & exit /b 1)
echo copying DFEngine_orig.dll over DFEngine.dll
copy /Y "%GAME%\DFEngine_orig.dll" "%GAME%\DFEngine.dll" >nul || (echo ERROR: copy failed & exit /b 1)
echo done. DFEngine_orig.dll, dxhrfix.ini and dxhrfix.log were left in place.
exit /b 0
