@echo off
REM ══════════════════════════════════════════════════════════════════════════════
REM  build.bat  —  Cipher Engine Launcher v2.0 build script
REM  Run from: "x64 Native Tools Command Prompt for VS 2022"
REM
REM  BEFORE BUILDING, put these files in the res\ folder:
REM    res\fairy-stockfish.exe          ← Fairy-Stockfish x64 binary
REM    res\nn-46832cfbead3.nnue.xz      ← NNUE compressed with: xz -9e --keep file.nnue
REM    res\xz.exe                       ← from xz-5.x.x-windows\bin_x86-64\xz.exe
REM    res\liblzma.dll                  ← from xz-5.x.x-windows\bin_x86-64\liblzma.dll
REM    icons\cipher.ico                 ← your app icon
REM ══════════════════════════════════════════════════════════════════════════════
setlocal

REM ── Sanity checks ────────────────────────────────────────────────────────────
if not exist "res\fairy-stockfish.exe" (
    echo [FAIL] res\fairy-stockfish.exe not found.
    echo        Copy fairy-stockfish.exe into the res\ folder first.
    pause & exit /b 1
)
if not exist "res\nn-46832cfbead3.nnue.xz" (
    echo [FAIL] res\nn-46832cfbead3.nnue.xz not found.
    echo        Compress the NNUE file with:  xz -9e --keep nn-46832cfbead3.nnue
    echo        Then copy the .xz into the res\ folder.
    pause & exit /b 1
)
if not exist "res\xz.exe" (
    echo [FAIL] res\xz.exe not found.
    echo        Copy bin_x86-64\xz.exe from your xz-5.x.x-windows download into res\
    pause & exit /b 1
)
if not exist "res\liblzma.dll" (
    echo [FAIL] res\liblzma.dll not found.
    echo        Copy bin_x86-64\liblzma.dll from your xz-5.x.x-windows download into res\
    pause & exit /b 1
)
if not exist "icons\cipher.ico" (
    echo [WARN] icons\cipher.ico not found — build continues without icon.
)

echo.
echo  [1/3] Compiling resources ...
rc /nologo /fo cipher.res cipher.rc
if errorlevel 1 (
    echo [FAIL] rc.exe failed. Check cipher.rc for errors.
    pause & exit /b 1
)
echo  [  OK  ] cipher.res

echo.
echo  [2/3] Compiling and linking ...
cl /nologo /EHsc /O2 /Oi /Ot /GL /W3 /DUNICODE /D_UNICODE  ^
   /GS /sdl /guard:cf                                        ^
   cipher_launcher.cpp                                       ^
   cipher.res                                                ^
   /link                                                     ^
   /LTCG                                                     ^
   /OPT:REF /OPT:ICF                                        ^
   /DEBUG:NONE                                               ^
   /guard:cf                                                 ^
   advapi32.lib shell32.lib user32.lib                       ^
   ws2_32.lib iphlpapi.lib                                   ^
   /SUBSYSTEM:WINDOWS                                        ^
   /OUT:CipherLauncher.exe
if errorlevel 1 (
    echo.
    echo [FAIL] Compilation failed. See errors above.
    pause & exit /b 1
)

echo.
echo  [3/3] Stripping PE timestamp ...
powershell -NoProfile -Command ^
  "$f='CipherLauncher.exe';" ^
  "$b=[IO.File]::ReadAllBytes($f);" ^
  "$pe=[BitConverter]::ToInt32($b,0x3C);" ^
  "$b[$pe+8]=$b[$pe+9]=$b[$pe+10]=$b[$pe+11]=0;" ^
  "[IO.File]::WriteAllBytes($f,$b)"
if errorlevel 1 (
    echo [WARN] Timestamp strip failed — non-fatal, continuing.
) else (
    echo  [  OK  ] PE timestamp zeroed
)

echo.
echo  ══════════════════════════════════════════════════
echo   Build successful:  CipherLauncher.exe
echo   Size:
for %%F in (CipherLauncher.exe) do echo    %%~zF bytes
echo  ══════════════════════════════════════════════════
echo.
echo  Distribute ONLY CipherLauncher.exe — no other files needed.
echo.
pause
endlocal