Cipher Engine Launcher v2.0 — Build Instructions
══════════════════════════════════════════════════

WHAT CHANGED FROM v1
─────────────────────
v1: CipherLauncher.exe → downloads Python + pip + Fairy-Stockfish + NNUE
v2: CipherLauncher.exe → ships with everything embedded inside the binary
    • No Python required
    • No downloads on the user's machine
    • No internet connection needed at runtime
    • Single .exe that a user double-clicks and it just works


HOW IT WORKS
────────────
First run:
  [1/3] Extract fairy-stockfish.exe from embedded resource → %APPDATA%\Cipher\
  [2/3] XZ-decompress NNUE (nn-46832cfbead3.nnue) in-place → %APPDATA%\Cipher\
  [3/3] Register CipherLauncher.exe in Windows startup

Then immediately:
  Start the built-in C++ WebSocket server on ws://localhost:8765
  Drive Fairy-Stockfish via UCI pipes
  Handle analyze/configure/ping messages from the Chrome extension


PROJECT LAYOUT
──────────────
  cipher_launcher.cpp     ← main source (v2, no Python)
  cipher.rc               ← resources: icon + embedded binaries
  build.bat               ← MSVC build script
  compress_nnue.bat       ← compress NNUE → res\*.nnue.xz
  icons\
    cipher.ico            ← your app icon (already here)
  res\                    ← YOU MUST CREATE and populate this folder
    fairy-stockfish.exe   ← copy from C:\Users\algorithm\AppData\Roaming\Cipher\
    nn-46832cfbead3.nnue.xz ← created by compress_nnue.bat


STEP-BY-STEP BUILD
──────────────────
1. Create the res\ folder next to cipher.rc if it does not exist.

2. Copy fairy-stockfish.exe into res\:
     copy "C:\Users\algorithm\AppData\Roaming\Cipher\fairy-stockfish.exe" res\

3. Compress the NNUE file. Run compress_nnue.bat — it handles xz or 7-Zip automatically.
   Or do it manually:
     winget install tukaani.xz         (if xz not installed)
     copy "C:\Users\algorithm\AppData\Roaming\Cipher\nn-46832cfbead3.nnue" res\nn-46832cfbead3.nnue
     xz -9e --keep res\nn-46832cfbead3.nnue
     del res\nn-46832cfbead3.nnue

   You should now have:  res\nn-46832cfbead3.nnue.xz  (~70-80 MB compressed)

4. Open "x64 Native Tools Command Prompt for VS 2022"

5. cd to this folder, then:
     build.bat

6. CipherLauncher.exe is your distributable. That single file is everything.


BINARY SIZE EXPECTATION
────────────────────────
  fairy-stockfish.exe    ~30 MB (runtime extracted, not stored compressed in exe)
  nn-46832cfbead3.nnue.xz ~75 MB (decompresses to ~250 MB on first run)
  Total exe:             ~105 MB + overhead

  On first run the user sees:
    [1/3] Extracting Fairy-Stockfish ...     [  OK  ]  fairy-stockfish.exe extracted
    [2/3] Extracting NNUE weights ...        [XZ] Decompressing 74MB → 250MB ...
                                             [  OK  ]  NNUE decompressed and saved
    [3/3] Windows startup                    [  OK  ]  Registered in Windows startup


WEBSOCKET PROTOCOL (unchanged from v1)
───────────────────────────────────────
Client → Server (JSON text frames):

  Analyze:
    {"type":"analyze","moves":["e2e4","e7e5"],"movetime":100}

  Configure variant:
    {"type":"configure","variant":"standard","nnue":"nn-46832cfbead3.nnue"}

  Ping:
    {"type":"ping"}

Server → Client:

  Best move:
    {"type":"bestmove","from":"e2","to":"e4","move":"e2e4"}

  Pong:
    {"type":"pong"}

  Error:
    {"type":"error","message":"..."}


WINDOWS VERSION SUPPORT
────────────────────────
Requires Windows 8+ (for ntdll LZMA decompression via RtlDecompressBufferEx).
Windows 7 will fail the NNUE decompression step but still run with default eval.

TROUBLESHOOTING
────────────────
"IDR_NNUE_XZ resource not found"
  → res\nn-46832cfbead3.nnue.xz was not added before rc.exe ran. Redo steps 2-5.

"LZMA2 decode failed / ntdll LZMA status: 0x..."
  → The .xz file is corrupt or not standard LZMA2. Recompress with: xz -9e

"bind() failed on port 8765"
  → Another process already owns the port. The launcher auto-kills it on startup.
    If it persists: netstat -ano | findstr 8765  then  taskkill /PID <pid> /F

"Failed to start Fairy-Stockfish"
  → The embedded binary may be for a different architecture.
    Use the x86-64 Fairy-Stockfish build on 64-bit Windows.
