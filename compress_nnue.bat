@echo off
REM ══════════════════════════════════════════════════════════════════════════════
REM  compress_nnue.bat — compress nn-46832cfbead3.nnue into res\*.nnue.xz
REM
REM  Run this ONCE after placing fairy-stockfish.exe in C:\Users\algorithm\AppData\Roaming\Cipher\
REM  It will read the .nnue from that path and output res\nn-46832cfbead3.nnue.xz
REM
REM  Requires: xz.exe on PATH  (install via: winget install tukaani.xz)
REM         OR 7-Zip (7z.exe) on PATH  (https://7-zip.org)
REM ══════════════════════════════════════════════════════════════════════════════

setlocal

set NNUE_SRC=C:\Users\algorithm\AppData\Roaming\Cipher\nn-46832cfbead3.nnue
set NNUE_DST=res\nn-46832cfbead3.nnue.xz

if not exist "res" mkdir res

if not exist "%NNUE_SRC%" (
    echo [FAIL] NNUE not found at: %NNUE_SRC%
    echo        Adjust NNUE_SRC at the top of this script.
    pause & exit /b 1
)

echo [....] Source: %NNUE_SRC%
echo [....] Output: %NNUE_DST%
echo.

REM ── Try xz first ─────────────────────────────────────────────────────────────
where xz >nul 2>&1
if %errorlevel%==0 (
    echo [....] Using xz ...
    copy /Y "%NNUE_SRC%" "res\nn-46832cfbead3.nnue" >nul
    xz -9e --keep "res\nn-46832cfbead3.nnue"
    del "res\nn-46832cfbead3.nnue"
    goto done
)

REM ── Try 7-Zip ────────────────────────────────────────────────────────────────
where 7z >nul 2>&1
if %errorlevel%==0 (
    echo [....] Using 7-Zip ...
    7z a -txz -mx=9 "%NNUE_DST%" "%NNUE_SRC%"
    goto done
)

REM ── Try 7-Zip at default install path ────────────────────────────────────────
if exist "C:\Program Files\7-Zip\7z.exe" (
    echo [....] Using 7-Zip at default path ...
    "C:\Program Files\7-Zip\7z.exe" a -txz -mx=9 "%NNUE_DST%" "%NNUE_SRC%"
    goto done
)

echo [FAIL] Neither xz nor 7-Zip found.
echo        Install xz:    winget install tukaani.xz
echo        Install 7-Zip: https://7-zip.org
pause & exit /b 1

:done
if exist "%NNUE_DST%" (
    echo.
    echo [  OK  ] Created: %NNUE_DST%
    for %%F in ("%NNUE_DST%") do echo         Size: %%~zF bytes
) else (
    echo [FAIL] Output file not created. Check errors above.
    pause & exit /b 1
)

echo.
echo  Now copy fairy-stockfish.exe to res\fairy-stockfish.exe and run build.bat
echo.
pause
endlocal
