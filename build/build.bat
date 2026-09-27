@echo off
REM ============================================================
REM  KarnySnap - build.bat
REM  Kompilacja pod Dell Latitude E5500 / Core 2 Duo T7250
REM  GCC 16.2.0, standard C23, optymalizacja pod rdzen Core2.
REM
REM  Uzycie:
REM     build.bat            -> buduje wersja dla biezacego gcc z PATH
REM     build.bat x86        -> wymusza kompilator i686-w64-mingw32-gcc
REM     build.bat x64        -> wymusza kompilator x86_64-w64-mingw32-gcc
REM ============================================================

setlocal
set SRC=..\src\karnysnap.c
set OUT=KarnySnap.exe
set FLAGS=-std=c23 -O2 -march=core2 -msse3 -mfpmath=sse -mwindows -Wall -Wextra
set LIBS=-lgdiplus -lshell32 -lole32 -lcomdlg32

if "%1"=="x86" (
    set CC=i686-w64-mingw32-gcc
    set OUT=KarnySnap_x86.exe
) else if "%1"=="x64" (
    set CC=x86_64-w64-mingw32-gcc
    set OUT=KarnySnap_x64.exe
) else (
    set CC=gcc
)

echo [KarnySnap] Kompilator: %CC%
echo [KarnySnap] Flagi:      %FLAGS%
echo.

%CC% %FLAGS% %SRC% -o %OUT% %LIBS%

if errorlevel 1 (
    echo.
    echo [KarnySnap] BLAD kompilacji.
    exit /b 1
)

echo.
echo [KarnySnap] Zbudowano: %OUT%
endlocal
