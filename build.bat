@echo off
REM Build FolderSize (x64) with MinGW-w64
REM ASCII only - keep this file free of non-ASCII characters.
setlocal

REM ---- locate g++ -------------------------------------------------
REM Look up PATH first, then a few common install locations.
set GXX=
for %%I in (g++.exe) do set GXX=%%~$PATH:I
if "%GXX%"=="" if exist "C:\mingw64\bin\g++.exe"        set GXX=C:\mingw64\bin\g++.exe
if "%GXX%"=="" if exist "C:\msys64\mingw64\bin\g++.exe" set GXX=C:\msys64\mingw64\bin\g++.exe
if "%GXX%"=="" if exist "C:\MinGW\bin\g++.exe"          set GXX=C:\MinGW\bin\g++.exe

if "%GXX%"=="" (
    echo [ERROR] g++ not found.
    echo         Install MinGW-w64, or set GXX to the full path of g++.exe
    echo         near the top of this file.
    if "%~1"=="" pause
    exit /b 1
)

if not exist "%~dp0bin" mkdir "%~dp0bin"

echo Compiling with: %GXX%
echo.

echo [1/3] FolderSizeCLI.exe  (scanner)
"%GXX%" -std=c++17 -O2 -Wall -Wextra -municode ^
    -o "%~dp0bin\FolderSizeCLI.exe" ^
    "%~dp0src\FolderSizeCLI.cpp" ^
    -static-libgcc -static-libstdc++
if errorlevel 1 goto fail

echo [2/3] FolderSizeExt.dll   (legacy column handler, unused on Win11)
"%GXX%" -std=c++17 -O2 -Wall -Wextra -shared ^
    -o "%~dp0bin\FolderSizeExt.dll" ^
    "%~dp0src\FolderSizeExt.cpp" ^
    -static-libgcc -static-libstdc++ ^
    -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lcomctl32 -ladvapi32 -luser32
if errorlevel 1 goto fail

echo [3/3] test.exe             (COM self-test)
"%GXX%" -std=c++17 -O2 -municode ^
    -o "%~dp0bin\test.exe" ^
    "%~dp0src\test.cpp" ^
    -static-libgcc -static-libstdc++ ^
    -lole32 -loleaut32 -luuid -lshell32 -lshlwapi
if errorlevel 1 echo [WARN] test.exe failed, the rest is still usable.

echo.
echo [OK] Build finished.
echo.
echo   Install : install.bat
echo   Scan    : bin\FolderSizeCLI.exe "C:\some\folder" out.tsv
if "%~1"=="" pause
exit /b 0

:fail
echo.
echo [FAILED] Build error.
if "%~1"=="" pause
exit /b 1
