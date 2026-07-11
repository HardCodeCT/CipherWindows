/*
 ══════════════════════════════════════════════════════════════════════════════
  cipher_launcher.cpp  –  Cipher Engine Launcher for Windows  (v3.0)
  Pure C++ — no Python, no downloads required.

  WHAT'S NEW IN v3.0 — ENGINE POOL + PARALLEL ANALYSIS
  ─────────────────────────────────────────────────────
  v2.0 used a single Fairy-Stockfish process protected by one mutex.
  Every analyze request from every browser tab had to queue behind the
  previous one — even when the PC had 8+ cores doing nothing.

  v3.0 replaces this with an EnginePool:

    • POOL_SIZE = min(hardware_concurrency, MAX_POOL) Fairy-Stockfish
      processes are launched at startup, each fully UCI-initialized and
      ready to receive "go" immediately.

    • Each WebSocket client grabs a free engine from the pool via
      AcquireEngine(), does its analysis, then returns it via
      ReleaseEngine(). If all engines are busy the request blocks on a
      condition_variable — no polling, no spin-wait.

    • Every engine in the pool gets:
        - Threads = max(1, totalCores / poolSize)   per-engine thread count
        - Hash    = max(32, totalRAM_MB / poolSize / 4)  MB hash per engine
      so the whole fleet uses the machine's resources efficiently without
      each engine starving the others.

    • The pool is shared across all WebSocket connections. Each connection
      is handled in its own std::thread (unchanged from v2.0), so latency
      for one client never blocks another client from acquiring a free engine.

    • A warm-pool strategy: after returning an engine the pool sends
      "isready" and waits for "readyok" in the background, so the engine's
      internal state is clean and its hash is pre-warmed for the next job.

    • ucinewgame is only sent when the variant or NNUE file changes, not on
      every request. This preserves the transposition table between moves of
      the same game, giving effectively deeper search at the same movetime.

  PROACTIVE / NOTPAID stealth cycle is unchanged from v2.0.

  BUILD  (x64 Native Tools Command Prompt for VS 2022)
  ──────
    rc /fo cipher.res cipher.rc

    cl /EHsc /O2 /W3 /DUNICODE /D_UNICODE                          ^
       cipher_launcher.cpp                                          ^
       cipher.res                                                   ^
       /link advapi32.lib shell32.lib user32.lib ws2_32.lib         ^
              iphlpapi.lib winhttp.lib                              ^
       /SUBSYSTEM:WINDOWS /OUT:CipherLauncher.exe
 ══════════════════════════════════════════════════════════════════════════════
*/

// ── compiler pragmas ─────────────────────────────────────────────────────────
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "winhttp.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX          // ← fix: prevents windows.h min/max macros from
                          //         breaking std::min, std::max and HICON exprs
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif
#define WINVER       0x0601
#define _WIN32_WINNT 0x0601
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <windows.h>
#include <psapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <intrin.h>

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <chrono>

// ══════════════════════════════════════════════════════════════════════════════
//  Resource IDs  (must match cipher.rc)
// ══════════════════════════════════════════════════════════════════════════════
#define IDR_STOCKFISH   200
#define IDR_NNUE_XZ     201
#define IDR_XZ_EXE      202
#define IDR_XZ_DLL      203

// ══════════════════════════════════════════════════════════════════════════════
//  Configuration
// ══════════════════════════════════════════════════════════════════════════════
namespace Cfg {
    const wchar_t* APP_DIR_NAME     = L"Cipher";
    const wchar_t* SF_EXE           = L"fairy-stockfish.exe";
    const wchar_t* NNUE_FILE        = L"nn-46832cfbead3.nnue";
    const wchar_t* NNUE_XZ_FILE     = L"nn-46832cfbead3.nnue.xz";
    const wchar_t* LAUNCHER_EXE     = L"CipherLauncher.exe";
    const wchar_t* MARKER_FILE      = L"installed.marker";
    const wchar_t* PID_FILE         = L"engine.pid";
    const wchar_t* RUN_KEY_NAME     = L"CipherEngine";
    const wchar_t* PROACTIVE_MARKER = L"proactive.marker";
    const int      ENGINE_PORT      = 8765;
    const int      DEFAULT_MOVETIME = 100;

    // Pool sizing limits — the pool auto-tunes within these bounds
    const int MAX_POOL_SIZE          = 1;   // single engine only — never spawn more than one SF process
    const int MIN_POOL_SIZE          = 1;
    const int MIN_THREADS_PER_ENGINE = 1;
    const int MAX_THREADS_PER_ENGINE = 64;  // effectively uncapped — single engine uses all cores
    const int MIN_HASH_MB            = 32;
    const int MAX_HASH_MB            = 512; // per engine
}

// ══════════════════════════════════════════════════════════════════════════════
//  Console / logging
// ══════════════════════════════════════════════════════════════════════════════
static bool g_console   = false;
static bool g_proactive = false;

static void EnsureConsole() {
    if (!g_console) {
        AllocConsole();
        FILE* _;
        freopen_s(&_, "CONOUT$", "w", stdout);
        freopen_s(&_, "CONOUT$", "w", stderr);
        freopen_s(&_, "CONIN$",  "r", stdin);
        g_console = true;

        if (g_proactive) {
            HWND hwnd = GetConsoleWindow();
            if (hwnd) {
                SetWindowTextW(hwnd, L"Mozilla Firefox");
                LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
                SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
                ShowWindow(hwnd, SW_HIDE);

                wchar_t exeDir[MAX_PATH];
                GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
                std::wstring dir(exeDir);
                size_t lastSep = dir.find_last_of(L"\\/");
                if (lastSep != std::wstring::npos) dir.resize(lastSep);
                std::wstring iconPath = dir + L"\\icons\\firefox.ico";
                if (GetFileAttributesW(iconPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    HICON big    = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                                     0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
                    HICON hSmall = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXSMICON),
                                                     GetSystemMetrics(SM_CYSMICON),
                                                     LR_LOADFROMFILE);
                    if (big)    SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)big);
                    if (hSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
                }
            }
        } else {
            SetConsoleTitleW(L"Cipher Engine");
        }

        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode))
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

static void Log(const wchar_t* fmt, ...) {
    EnsureConsole();
    wchar_t buf[2048];
    va_list args; va_start(args, fmt);
    vswprintf_s(buf, _countof(buf), fmt, args);
    va_end(args);
    wprintf(L"[%s] %s\n", g_proactive ? L"Firefox" : L"Cipher", buf);
}
static void LogOK  (const wchar_t* msg) { EnsureConsole(); wprintf(L"  \x1b[32m[  OK  ]\x1b[0m  %s\n", msg); }
static void LogFail(const wchar_t* msg) { EnsureConsole(); wprintf(L"  \x1b[31m[ FAIL ]\x1b[0m  %s\n", msg); }
static void LogStep(const wchar_t* msg) { EnsureConsole(); wprintf(L"  \x1b[36m[ .... ]\x1b[0m  %s\n", msg); }
static void LogInfo(const char*    msg) { EnsureConsole(); printf ("  %s\n", msg); }


// ══════════════════════════════════════════════════════════════════════════════
//  Path helpers
// ══════════════════════════════════════════════════════════════════════════════
static std::wstring GetAppDir() {
    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
        GetTempPathW(MAX_PATH, appdata);
    std::wstring dir = std::wstring(appdata) + L"\\" + Cfg::APP_DIR_NAME;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
static std::wstring Join(const std::wstring& dir, const wchar_t* name) {
    return dir + L"\\" + name;
}
static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
}
static bool FileReady(const std::wstring& p, DWORD minBytes = 1024) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
    ULARGE_INTEGER sz;
    sz.LowPart  = d.nFileSizeLow;
    sz.HighPart = d.nFileSizeHigh;
    return sz.QuadPart >= minBytes;
}
static std::wstring GetExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}
static bool IsInstalled(const std::wstring& appDir) {
    return FileExists(Join(appDir, Cfg::MARKER_FILE));
}
static void WriteMarker(const std::wstring& appDir) {
    std::ofstream f(Join(appDir, Cfg::MARKER_FILE), std::ios::trunc);
    if (f) f << "installed";
}
static void RemoveMarker(const std::wstring& appDir) {
    DeleteFileW(Join(appDir, Cfg::MARKER_FILE).c_str());
}

static void RemoveFromStartup() {
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) return;
    if (RegDeleteValueW(hk, Cfg::RUN_KEY_NAME) == ERROR_SUCCESS)
        LogOK(L"Removed stale Windows startup entry (HKCU\\Run)");
    RegCloseKey(hk);
}


// ══════════════════════════════════════════════════════════════════════════════
//  Machine capacity — used for auto-tuning pool and per-engine resources
// ══════════════════════════════════════════════════════════════════════════════
struct MachineSpec {
    int    logicalCores;
    size_t totalRAM_MB;
};

static MachineSpec QueryMachine() {
    MachineSpec s{};
    s.logicalCores = (int)std::thread::hardware_concurrency();
    if (s.logicalCores < 1) s.logicalCores = 1;

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
        s.totalRAM_MB = (size_t)(mem.ullTotalPhys / (1024 * 1024));
    else
        s.totalRAM_MB = 2048; // safe fallback

    return s;
}

// Decide how many engines to put in the pool and how to configure each one.
struct PoolConfig {
    int poolSize;
    int threadsPerEngine;
    int hashMB;
};

static PoolConfig ComputePoolConfig(const MachineSpec& m) {
    PoolConfig c{};

    // Single-engine mode: always exactly one Fairy-Stockfish process.
    c.poolSize = 1;

    // That one engine gets all logical cores as its Threads value,
    // clamped to [MIN_THREADS_PER_ENGINE, MAX_THREADS_PER_ENGINE].
    c.threadsPerEngine = std::max(Cfg::MIN_THREADS_PER_ENGINE,
                          std::min(Cfg::MAX_THREADS_PER_ENGINE, m.logicalCores));

    // Hash: 1/4 of total RAM, clamped. No division across engines since poolSize == 1.
    size_t hashBudget = m.totalRAM_MB / 4;
    c.hashMB = (int)std::max((size_t)Cfg::MIN_HASH_MB,
                             std::min((size_t)Cfg::MAX_HASH_MB, hashBudget));

    return c;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Resource extraction
// ══════════════════════════════════════════════════════════════════════════════
static bool ExtractResource(WORD resId, const std::wstring& destPath, const wchar_t* label) {
    HRSRC   hRsc = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hRsc) { LogFail((std::wstring(label) + L": FindResource failed").c_str()); return false; }
    HGLOBAL hMem = LoadResource(nullptr, hRsc);
    if (!hMem) { LogFail((std::wstring(label) + L": LoadResource failed").c_str()); return false; }
    DWORD sz   = SizeofResource(nullptr, hRsc);
    void* data = LockResource(hMem);
    if (!data || sz == 0) { LogFail((std::wstring(label) + L": empty resource").c_str()); return false; }

    HANDLE h = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogFail((std::wstring(label) + L": cannot create dest file").c_str()); return false;
    }
    DWORD written = 0;
    WriteFile(h, data, sz, &written, nullptr);
    CloseHandle(h);
    if (written != sz) { LogFail((std::wstring(label) + L": write incomplete").c_str()); return false; }
    LogOK((std::wstring(label) + L" extracted (" + std::to_wstring(sz) + L" bytes)").c_str());
    return true;
}


// ══════════════════════════════════════════════════════════════════════════════
//  NNUE decompression via embedded xz.exe
// ══════════════════════════════════════════════════════════════════════════════
static bool ExtractAndDecompressNNUE(const std::wstring& nnueDest) {
    LogStep(L"Decompressing NNUE weights via xz.exe ...");
    const std::wstring appDir = nnueDest.substr(0, nnueDest.find_last_of(L'\\'));

    const std::wstring xzExePath  = appDir + L"\\xz.exe";
    const std::wstring xzDllPath  = appDir + L"\\liblzma.dll";
    if (!FileExists(xzExePath))  { if (!ExtractResource(IDR_XZ_EXE,  xzExePath,  L"xz.exe"))      return false; }
    if (!FileExists(xzDllPath))  { if (!ExtractResource(IDR_XZ_DLL,  xzDllPath,  L"liblzma.dll")) return false; }

    const std::wstring xzFilePath = appDir + L"\\nn-46832cfbead3.nnue.xz";
    if (!FileExists(xzFilePath)) { if (!ExtractResource(IDR_NNUE_XZ, xzFilePath, L"nn-46832cfbead3.nnue.xz")) return false; }

    std::wstring cmd = L"\"" + xzExePath + L"\" -d --keep \"" + xzFilePath + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, appDir.c_str(), &si, &pi)) {
        LogFail(L"Failed to launch xz.exe");
        return false;
    }
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    if (waitResult != WAIT_OBJECT_0 || exitCode != 0) {
        LogFail(L"xz.exe decompression failed");
        return false;
    }
    if (!FileReady(nnueDest, 1024 * 1024)) {
        LogFail(L"Decompressed NNUE file missing or too small");
        return false;
    }
    DeleteFileW(xzFilePath.c_str());
    LogOK((L"NNUE decompressed → " + nnueDest).c_str());
    return true;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Minimal WebSocket helpers
// ══════════════════════════════════════════════════════════════════════════════
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const uint8_t* in, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; ) {
        uint32_t b = 0; int n = 0;
        for (; n < 3 && i < len; n++, i++) b = (b << 8) | in[i];
        b <<= (3 - n) * 8;
        out += B64[(b >> 18) & 63]; out += B64[(b >> 12) & 63];
        out += (n > 1) ? B64[(b >> 6) & 63] : '=';
        out += (n > 2) ? B64[(b & 63)]      : '=';
    }
    return out;
}

static void SHA1(const uint8_t* msg, size_t msgLen, uint8_t digest[20]) {
    uint32_t h[5] = { 0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0 };
    uint64_t bitLen = (uint64_t)msgLen * 8;
    std::vector<uint8_t> padded(msg, msg + msgLen);
    padded.push_back(0x80);
    while (padded.size() % 64 != 56) padded.push_back(0);
    for (int i = 7; i >= 0; i--) padded.push_back((uint8_t)(bitLen >> (i * 8)));
    for (size_t off = 0; off < padded.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)padded[off+i*4]<<24)|((uint32_t)padded[off+i*4+1]<<16)|
                   ((uint32_t)padded[off+i*4+2]<<8)|(uint32_t)padded[off+i*4+3];
        for (int i = 16; i < 80; i++) { uint32_t x=w[i-3]^w[i-8]^w[i-14]^w[i-16]; w[i]=(x<<1)|(x>>31); }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f,k;
            if      (i<20){f=(b&c)|(~b&d);k=0x5A827999;}
            else if (i<40){f=b^c^d;       k=0x6ED9EBA1;}
            else if (i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else          {f=b^c^d;       k=0xCA62C1D6;}
            uint32_t temp=((a<<5)|(a>>27))+f+e+k+w[i];
            e=d;d=c;c=(b<<30)|(b>>2);b=a;a=temp;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    }
    for (int i=0;i<5;i++){digest[i*4+0]=(uint8_t)(h[i]>>24);digest[i*4+1]=(uint8_t)(h[i]>>16);
                           digest[i*4+2]=(uint8_t)(h[i]>>8); digest[i*4+3]=(uint8_t)h[i];}
}

static bool WsSendText(SOCKET s, const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81);
    size_t len = payload.size();
    if (len < 126) { frame.push_back((uint8_t)len); }
    else if (len < 65536) { frame.push_back(126); frame.push_back((uint8_t)(len>>8)); frame.push_back((uint8_t)(len&0xFF)); }
    else { frame.push_back(127); for (int i=7;i>=0;i--) frame.push_back((uint8_t)(len>>(i*8))); }
    frame.insert(frame.end(), payload.begin(), payload.end());
    int sent = send(s,(const char*)frame.data(),(int)frame.size(),0);
    return sent == (int)frame.size();
}

static bool WsSendClose(SOCKET s) {
    uint8_t frame[2]={0x88,0x00};
    send(s,(const char*)frame,2,0);
    return true;
}

static bool WsReadFrame(SOCKET s, std::string& payload, bool& isClose) {
    isClose = false; payload.clear();
    auto recvAll = [&](uint8_t* buf, int n) -> bool {
        while (n > 0) { int r=recv(s,(char*)buf,n,MSG_WAITALL); if (r<=0) return false; buf+=r;n-=r; }
        return true;
    };
    uint8_t hdr[2]; if (!recvAll(hdr,2)) return false;
    uint8_t opcode = hdr[0]&0x0F;
    bool masked = (hdr[1]&0x80)!=0;
    uint64_t plen = hdr[1]&0x7F;
    if (opcode==0x08){isClose=true;return false;}
    if (opcode==0x09){ uint8_t pong[2]={0x8A,0x00};send(s,(const char*)pong,2,0);return WsReadFrame(s,payload,isClose); }
    if (plen==126){ uint8_t ext[2];if(!recvAll(ext,2))return false;plen=((uint64_t)ext[0]<<8)|ext[1]; }
    else if (plen==127){ uint8_t ext[8];if(!recvAll(ext,8))return false;plen=0;for(int i=0;i<8;i++)plen=(plen<<8)|ext[i]; }
    uint8_t mask[4]={};
    if (masked&&!recvAll(mask,4)) return false;
    if (plen > 16*1024*1024) return false;
    std::vector<uint8_t> data((size_t)plen);
    if (plen>0&&!recvAll(data.data(),(int)plen)) return false;
    if (masked) for (size_t i=0;i<(size_t)plen;i++) data[i]^=mask[i&3];
    payload=std::string(data.begin(),data.end());
    return true;
}

static bool WsHandshake(SOCKET s) {
    char buf[4096]={};
    int total=0;
    while (total<(int)sizeof(buf)-1) {
        int r=recv(s,buf+total,1,0); if(r<=0) return false;
        total++;
        if (total>=4&&memcmp(buf+total-4,"\r\n\r\n",4)==0) break;
    }
    std::string req(buf);
    std::string keyHeader="Sec-WebSocket-Key: ";
    size_t pos=req.find(keyHeader); if(pos==std::string::npos) return false;
    pos+=keyHeader.size();
    size_t end=req.find("\r\n",pos); if(end==std::string::npos) return false;
    std::string key=req.substr(pos,end-pos);
    static const char GUID[]="258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined=key+GUID;
    uint8_t digest[20];
    SHA1(reinterpret_cast<const uint8_t*>(combined.data()),combined.size(),digest);
    std::string accept=Base64Encode(digest,20);
    std::string response=
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: "+accept+"\r\n\r\n";
    send(s,response.data(),(int)response.size(),0);
    return true;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Minimal JSON helpers
// ══════════════════════════════════════════════════════════════════════════════
static std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string needle="\""+key+"\"";
    size_t pos=json.find(needle); if(pos==std::string::npos) return {};
    pos=json.find(':',pos+needle.size()); if(pos==std::string::npos) return {};
    pos=json.find('"',pos+1); if(pos==std::string::npos) return {};
    size_t end=json.find('"',pos+1); if(end==std::string::npos) return {};
    return json.substr(pos+1,end-pos-1);
}
static int JsonGetInt(const std::string& json, const std::string& key, int def) {
    std::string needle="\""+key+"\"";
    size_t pos=json.find(needle); if(pos==std::string::npos) return def;
    pos=json.find(':',pos+needle.size()); if(pos==std::string::npos) return def;
    while(pos<json.size()&&(json[pos]==':'||json[pos]==' ')) pos++;
    if(pos>=json.size()) return def;
    try { return std::stoi(json.substr(pos)); } catch(...) { return def; }
}
static std::vector<std::string> JsonGetArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string needle="\""+key+"\"";
    size_t pos=json.find(needle); if(pos==std::string::npos) return result;
    pos=json.find('[',pos+needle.size()); if(pos==std::string::npos) return result;
    size_t end=json.find(']',pos); if(end==std::string::npos) return result;
    std::string arr=json.substr(pos+1,end-pos-1);
    for (size_t i=0;i<arr.size();) {
        size_t q1=arr.find('"',i); if(q1==std::string::npos) break;
        size_t q2=arr.find('"',q1+1); if(q2==std::string::npos) break;
        result.push_back(arr.substr(q1+1,q2-q1-1));
        i=q2+1;
    }
    return result;
}
static std::string JsonString(const std::string& key, const std::string& val) {
    return "\""+key+"\":\""+val+"\"";
}
static std::string JsonObj(std::initializer_list<std::string> kvPairs) {
    std::string out="{";
    for (auto& s:kvPairs) { if(out.size()>1) out+=","; out+=s; }
    return out+"}";
}


// ══════════════════════════════════════════════════════════════════════════════
//  Single Fairy-Stockfish process — UCI wrapper
// ══════════════════════════════════════════════════════════════════════════════
struct Engine {
    int          id         = 0;
    std::string  variant    = "standard";
    std::string  nnue       = "nn-46832cfbead3.nnue";
    HANDLE       hIn        = INVALID_HANDLE_VALUE;
    HANDLE       hOut       = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi  = {};
    std::string  appDir;
    std::wstring sfPathW;
    std::wstring appDirW;

    // Per-engine resource config (set by pool before Start())
    int threadsCount = 1;
    int hashMB       = 64;

    // Track last variant so we only restart when it actually changes
    std::string lastVariant;
    std::string lastNnue;
    bool        needsNewGame = true;

    bool Start(const std::wstring& sfPath, const std::wstring& appDirIn) {
        sfPathW  = sfPath;
        appDirW  = appDirIn;
        int n = WideCharToMultiByte(CP_UTF8,0,appDirIn.c_str(),-1,nullptr,0,nullptr,nullptr);
        appDir.resize(n-1);
        WideCharToMultiByte(CP_UTF8,0,appDirIn.c_str(),-1,&appDir[0],n,nullptr,nullptr);
        return Spawn(sfPath, appDirIn);
    }

    bool IsAlive() {
        if (!pi.hProcess) return false;
        DWORD code = STILL_ACTIVE;
        GetExitCodeProcess(pi.hProcess, &code);
        return code == STILL_ACTIVE;
    }

    void Send(const std::string& cmd) {
        std::string line = cmd+"\n";
        DWORD w=0;
        WriteFile(hIn, line.data(), (DWORD)line.size(), &w, nullptr);
    }

    std::string ReadLine() {
        std::string line; char c; DWORD r;
        while (ReadFile(hOut,&c,1,&r,nullptr)&&r>0) {
            if (c=='\n') break;
            if (c!='\r') line+=c;
        }
        return line;
    }

    // Drain output until we see the token or hit the attempt limit
    bool AwaitToken(const std::string& token, int maxAttempts = 300) {
        for (int i = 0; i < maxAttempts; i++) {
            if (ReadLine() == token) return true;
        }
        return false;
    }

    void Configure() {
        Send("uci");
        if (!AwaitToken("uciok")) throw std::runtime_error("uciok timeout");
        ApplyOptions();
        Send("isready");
        if (!AwaitToken("readyok")) throw std::runtime_error("readyok timeout");
        lastVariant  = variant;
        lastNnue     = nnue;
        needsNewGame = true;
        printf("[Engine %d] ready — variant=%s threads=%d hash=%dMB\n",
               id, variant.c_str(), threadsCount, hashMB);
    }

    void ApplyOptions(int elo = 2200) {
        Send("setoption name Use NNUE value true");
        Send("setoption name EvalFile value " + nnue);
        Send("setoption name UCI_Variant value " + variant);
        Send("setoption name Threads value " + std::to_string(threadsCount));
        Send("setoption name Hash value "    + std::to_string(hashMB));
        Send("setoption name UCI_LimitStrength value true");
        int clampedElo = std::max(500, std::min(elo, 2850));
        Send("setoption name UCI_Elo value " + std::to_string(clampedElo));
    }

    // Called by pool after returning an engine — re-arms it without flushing hash.
    // Only sends isready; no ucinewgame, so the TT stays warm.
    void WarmUp() {
        Send("isready");
        AwaitToken("readyok", 100);
    }

    // Reconfigure if variant/nnue changed. Only restarts the process when needed.
    // Returns false if restart failed.
    bool Reconfigure(const std::string& newVariant, const std::string& newNnue) {
        if (newVariant == lastVariant && newNnue == lastNnue) return true;

        printf("[Engine %d] reconfiguring %s→%s\n", id,
               lastVariant.c_str(), newVariant.c_str());
        variant = newVariant;
        nnue    = newNnue;

        // Kill current process and spawn a fresh one so UCI_Variant takes effect
        Close();
        if (!Spawn(sfPathW, appDirW)) {
            printf("[Engine %d] restart failed after reconfigure\n", id);
            return false;
        }
        needsNewGame = true;
        return true;
    }

    bool BestMove(const std::vector<std::string>& moves, int movetime,
                  std::string& fromSq, std::string& toSq) {
        // Send ucinewgame only when the position context needs to be reset
        if (needsNewGame) {
            Send("ucinewgame");
            needsNewGame = false;
        }
        std::string movesStr;
        for (auto& m : moves) { if (!movesStr.empty()) movesStr+=' '; movesStr+=m; }
        Send("position startpos moves " + movesStr);
        Send("go movetime " + std::to_string(movetime));
        while (true) {
            std::string line = ReadLine();
            if (line.rfind("bestmove",0)==0) {
                size_t sp=line.find(' ');
                if (sp==std::string::npos){fromSq=toSq="";return false;}
                std::string mv=line.substr(sp+1);
                size_t sp2=mv.find(' '); if(sp2!=std::string::npos) mv=mv.substr(0,sp2);
                if (mv.size()<4){fromSq=toSq="";return false;}
                fromSq=mv.substr(0,2); toSq=mv.substr(2,2);
                return true;
            }
        }
    }

    void Close() {
        if (IsAlive()) { Send("quit"); Sleep(200); }
        if (hIn !=INVALID_HANDLE_VALUE){CloseHandle(hIn); hIn =INVALID_HANDLE_VALUE;}
        if (hOut!=INVALID_HANDLE_VALUE){CloseHandle(hOut);hOut=INVALID_HANDLE_VALUE;}
        if (pi.hProcess){TerminateProcess(pi.hProcess,0);WaitForSingleObject(pi.hProcess,2000);CloseHandle(pi.hProcess);pi.hProcess=nullptr;}
        if (pi.hThread) {CloseHandle(pi.hThread); pi.hThread=nullptr;}
    }

private:
    bool Spawn(const std::wstring& sfPath, const std::wstring& workDir) {
        SECURITY_ATTRIBUTES sa{}; sa.nLength=sizeof(sa); sa.bInheritHandle=TRUE;
        HANDLE hChildInR,hChildInW,hChildOutR,hChildOutW;
        if (!CreatePipe(&hChildInR,&hChildInW,&sa,0)) return false;
        if (!CreatePipe(&hChildOutR,&hChildOutW,&sa,0)){CloseHandle(hChildInR);CloseHandle(hChildInW);return false;}
        SetHandleInformation(hChildInW, HANDLE_FLAG_INHERIT,0);
        SetHandleInformation(hChildOutR,HANDLE_FLAG_INHERIT,0);
        STARTUPINFOW si{};
        si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
        si.wShowWindow=SW_HIDE; si.hStdInput=hChildInR; si.hStdOutput=hChildOutW; si.hStdError=hChildOutW;
        ZeroMemory(&pi,sizeof(pi));
        std::wstring cmd=L"\""+sfPath+L"\"";
        BOOL ok=CreateProcessW(nullptr,&cmd[0],nullptr,nullptr,TRUE,CREATE_NO_WINDOW,
                               nullptr,workDir.c_str(),&si,&pi);
        CloseHandle(hChildInR); CloseHandle(hChildOutW);
        if (!ok){CloseHandle(hChildInW);CloseHandle(hChildOutR);return false;}
        hIn=hChildInW; hOut=hChildOutR;
        Configure();
        return true;
    }
};


// ══════════════════════════════════════════════════════════════════════════════
//  EnginePool — manages N Engine instances.
//
//  Design:
//   - Engines are pre-spawned at startup. No lazy init on the hot path.
//   - AcquireEngine() blocks on a condition_variable until one is free.
//     Max wait is bounded so a dead engine doesn't hang a client forever.
//   - ReleaseEngine() warms the engine up in a background thread (sends
//     "isready", awaits "readyok") so its internal state is clean and its
//     hash TT is ready for the next acquire. The engine is not returned to
//     the free queue until warmup completes.
//   - If an engine dies mid-use (crash, OOM) it is restarted transparently
//     before being returned to the pool.
// ══════════════════════════════════════════════════════════════════════════════
class EnginePool {
public:
    // Initialize the pool. Blocks until all engines are started.
    bool Init(int poolSize, int threadsPerEngine, int hashMB,
              const std::wstring& sfPath, const std::wstring& appDir) {
        sfPath_ = sfPath;
        appDir_ = appDir;

        printf("[Pool] Starting %d engine(s) — %d threads / %d MB hash each\n",
               poolSize, threadsPerEngine, hashMB);

        engines_.resize(poolSize);
        for (int i = 0; i < poolSize; i++) {
            auto e = std::make_unique<Engine>();
            e->id           = i;
            e->threadsCount = threadsPerEngine;
            e->hashMB       = hashMB;
            if (!e->Start(sfPath, appDir)) {
                printf("[Pool] Engine %d failed to start\n", i);
                return false;
            }
            engines_[i] = std::move(e);
            freeQueue_.push(i);
        }
        printf("[Pool] All %d engine(s) ready\n", poolSize);
        return true;
    }

    // Block until a free engine is available. Returns nullptr only on timeout.
    Engine* Acquire(int timeoutMs = 30000) {
        std::unique_lock<std::mutex> lk(mu_);
        bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                               [this]{ return !freeQueue_.empty(); });
        if (!ok) return nullptr;
        int idx = freeQueue_.front();
        freeQueue_.pop();
        return engines_[idx].get();
    }

    // Return an engine to the pool. Restarts it first if it crashed,
    // then warms it up in a detached background thread.
    void Release(Engine* e) {
        int idx = e->id;
        std::thread([this, e, idx]() {
            // Restart if the process died
            if (!e->IsAlive()) {
                printf("[Pool] Engine %d died — restarting ...\n", idx);
                e->Close();
                if (!e->Start(sfPath_, appDir_)) {
                    printf("[Pool] Engine %d restart failed\n", idx);
                }
            } else {
                // Just warm up — sends isready/readyok without flushing the hash
                e->WarmUp();
            }
            // Push back onto the free queue
            {
                std::lock_guard<std::mutex> lk(mu_);
                freeQueue_.push(idx);
            }
            cv_.notify_one();
        }).detach();
    }

    void CloseAll() {
        for (auto& e : engines_) if (e) e->Close();
    }

    int Size() const { return (int)engines_.size(); }

private:
    std::vector<std::unique_ptr<Engine>> engines_;
    std::queue<int>                      freeQueue_;
    std::mutex                           mu_;
    std::condition_variable              cv_;
    std::wstring                         sfPath_;
    std::wstring                         appDir_;
};

// Global pool — shared across all WebSocket connections
static EnginePool* g_pool = nullptr;


// ══════════════════════════════════════════════════════════════════════════════
//  Windows device fingerprint
// ══════════════════════════════════════════════════════════════════════════════
static std::string GetWindowsDeviceData() {
    std::string volserial, cpu;
    {
        DWORD serial=0;
        if (GetVolumeInformationW(L"C:\\",nullptr,0,&serial,nullptr,nullptr,nullptr,0)) {
            char buf[16]; snprintf(buf,sizeof(buf),"%08X",serial); volserial=buf;
        }
    }
    {
        char brand[49]={}; int info[4]={};
        __cpuid(info,0x80000000);
        if ((unsigned)info[0]>=0x80000004) {
            __cpuid(info,0x80000002);memcpy(brand,info,16);
            __cpuid(info,0x80000003);memcpy(brand+16,info,16);
            __cpuid(info,0x80000004);memcpy(brand+32,info,16);
            brand[48]='\0'; const char* p=brand; while(*p==' ')p++; cpu=std::string(p);
        }
    }
    if (volserial.empty()&&cpu.empty()) return {};
    return "vol="+volserial+"|cpu="+cpu;
}


// ══════════════════════════════════════════════════════════════════════════════
//  NNUE download via WinHTTP
// ══════════════════════════════════════════════════════════════════════════════
static bool DownloadNNUE(const std::wstring& appDir,
                          const std::string&  nnueFilename,
                          const std::string&  gdriveId) {
    int wn=MultiByteToWideChar(CP_UTF8,0,nnueFilename.c_str(),-1,nullptr,0);
    std::wstring wFilename(wn-1,L'\0');
    MultiByteToWideChar(CP_UTF8,0,nnueFilename.c_str(),-1,&wFilename[0],wn);
    std::wstring destPath = appDir+L"\\"+wFilename;

    int wid=MultiByteToWideChar(CP_UTF8,0,gdriveId.c_str(),-1,nullptr,0);
    std::wstring wId(wid-1,L'\0');
    MultiByteToWideChar(CP_UTF8,0,gdriveId.c_str(),-1,&wId[0],wid);
    std::wstring urlPath=L"/uc?id="+wId+L"&export=download&confirm=t";

    wprintf(L"  [NNUE] Downloading %s from Google Drive ...\n", wFilename.c_str());

    HINTERNET hSession=WinHttpOpen(L"CipherLauncher/3.0",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if (!hSession){LogFail(L"WinHttpOpen failed");return false;}
    HINTERNET hConnect=WinHttpConnect(hSession,L"drive.google.com",INTERNET_DEFAULT_HTTPS_PORT,0);
    if (!hConnect){WinHttpCloseHandle(hSession);LogFail(L"WinHttpConnect failed");return false;}
    HINTERNET hRequest=WinHttpOpenRequest(hConnect,L"GET",urlPath.c_str(),nullptr,
                                          WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if (!hRequest){WinHttpCloseHandle(hConnect);WinHttpCloseHandle(hSession);return false;}

    DWORD rp=WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest,WINHTTP_OPTION_REDIRECT_POLICY,&rp,sizeof(rp));
    if (!WinHttpSendRequest(hRequest,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||
        !WinHttpReceiveResponse(hRequest,nullptr)) {
        WinHttpCloseHandle(hRequest);WinHttpCloseHandle(hConnect);WinHttpCloseHandle(hSession);
        LogFail(L"WinHttpSendRequest failed");return false;
    }
    DWORD statusCode=0,statusSize=sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,&statusCode,&statusSize,WINHTTP_NO_HEADER_INDEX);
    if (statusCode!=200){
        wprintf(L"  [NNUE] HTTP %lu\n",statusCode);
        WinHttpCloseHandle(hRequest);WinHttpCloseHandle(hConnect);WinHttpCloseHandle(hSession);
        return false;
    }
    HANDLE hFile=CreateFileW(destPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (hFile==INVALID_HANDLE_VALUE){
        WinHttpCloseHandle(hRequest);WinHttpCloseHandle(hConnect);WinHttpCloseHandle(hSession);
        LogFail(L"Cannot create NNUE dest file");return false;
    }
    std::vector<char> buf(256*1024);
    DWORD totalBytes=0,bytesAvail=0,bytesRead=0;
    while (WinHttpQueryDataAvailable(hRequest,&bytesAvail)&&bytesAvail>0) {
        DWORD toRead = (bytesAvail < (DWORD)buf.size()) ? bytesAvail : (DWORD)buf.size();
        if (!WinHttpReadData(hRequest,buf.data(),toRead,&bytesRead)) break;
        DWORD written=0;
        WriteFile(hFile,buf.data(),bytesRead,&written,nullptr);
        totalBytes+=written;
    }
    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);WinHttpCloseHandle(hConnect);WinHttpCloseHandle(hSession);
    if (totalBytes==0){DeleteFileW(destPath.c_str());return false;}
    wprintf(L"  [NNUE] Downloaded %lu bytes\n",totalBytes);
    return true;
}

static bool EnsureNNUE(SOCKET clientSock, const std::wstring& appDir,
                        const std::string& nnueFilename, const std::string& gdriveId) {
    if (nnueFilename=="nn-46832cfbead3.nnue") return true;
    int wn=MultiByteToWideChar(CP_UTF8,0,nnueFilename.c_str(),-1,nullptr,0);
    std::wstring wFilename(wn-1,L'\0');
    MultiByteToWideChar(CP_UTF8,0,nnueFilename.c_str(),-1,&wFilename[0],wn);
    std::wstring destPath=appDir+L"\\"+wFilename;
    if (FileReady(destPath,1024*1024)){
        WsSendText(clientSock,JsonObj({JsonString("type","nnue_download_done"),JsonString("nnue",nnueFilename)}));
        return true;
    }
    WsSendText(clientSock,JsonObj({JsonString("type","nnue_download_start"),JsonString("nnue",nnueFilename)}));
    const int MAX_ATTEMPTS=3;
    const DWORD RETRY_DELAYS_MS[]={0,5000,15000};
    for (int attempt=1;attempt<=MAX_ATTEMPTS;attempt++){
        if (RETRY_DELAYS_MS[attempt-1]>0){
            WsSendText(clientSock,JsonObj({JsonString("type","nnue_download_retry"),
                JsonString("nnue",nnueFilename),JsonString("attempt",std::to_string(attempt)),
                JsonString("of",std::to_string(MAX_ATTEMPTS))}));
            Sleep(RETRY_DELAYS_MS[attempt-1]);
        }
        if (DownloadNNUE(appDir,nnueFilename,gdriveId)){
            WsSendText(clientSock,JsonObj({JsonString("type","nnue_download_done"),JsonString("nnue",nnueFilename)}));
            return true;
        }
        DeleteFileW(destPath.c_str());
    }
    WsSendText(clientSock,JsonObj({JsonString("type","nnue_download_error"),
        JsonString("nnue",nnueFilename),
        JsonString("message","Could not download the variant engine file after "
                  +std::to_string(MAX_ATTEMPTS)+" attempts. Check your network connection.")}));
    return false;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Proactive / NotPaid stealth (unchanged from v2.0)
// ══════════════════════════════════════════════════════════════════════════════
static void WriteProactiveMarker(const std::wstring& appDir, const std::wstring& originalExe) {
    std::ofstream f(Join(appDir,Cfg::PROACTIVE_MARKER),std::ios::trunc);
    if (f){
        int n=WideCharToMultiByte(CP_UTF8,0,originalExe.c_str(),-1,nullptr,0,nullptr,nullptr);
        std::string narrow(n-1,'\0');
        WideCharToMultiByte(CP_UTF8,0,originalExe.c_str(),-1,&narrow[0],n,nullptr,nullptr);
        f<<narrow;
    }
}
static std::wstring ReadProactiveMarker(const std::wstring& appDir) {
    std::ifstream f(Join(appDir,Cfg::PROACTIVE_MARKER)); if(!f) return {};
    std::string narrow; std::getline(f,narrow);
    int n=MultiByteToWideChar(CP_UTF8,0,narrow.c_str(),-1,nullptr,0);
    std::wstring wide(n-1,L'\0');
    MultiByteToWideChar(CP_UTF8,0,narrow.c_str(),-1,&wide[0],n);
    return wide;
}
static bool LaunchExeDetached(const std::wstring& exePath) {
    STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd=L"\""+exePath+L"\"";
    return CreateProcessW(nullptr,&cmd[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)!=0;
}

static bool HandleProactive(SOCKET clientSock, const std::wstring& appDir) {
    if (g_proactive){
        HWND hwnd=GetConsoleWindow(); if(hwnd) ShowWindow(hwnd,SW_HIDE);
        WsSendText(clientSock,JsonObj({JsonString("type","proactive_ok")}));
        return true;
    }
    std::wstring selfPath=GetExePath();
    std::wstring exeDir;
    size_t lastSep=selfPath.find_last_of(L"\\/");
    if (lastSep!=std::wstring::npos) exeDir=selfPath.substr(0,lastSep); else exeDir=L".";
    std::wstring firefoxPath=exeDir+L"\\firefox.exe";
    if (!CopyFileW(selfPath.c_str(),firefoxPath.c_str(),FALSE)){
        WsSendText(clientSock,JsonObj({JsonString("type","error"),JsonString("message","Could not create disguised process.")}));
        return false;
    }
    WriteProactiveMarker(appDir,selfPath);
    g_pool->CloseAll();
    if (!LaunchExeDetached(firefoxPath)){
        WsSendText(clientSock,JsonObj({JsonString("type","error"),JsonString("message","Could not start disguised process.")}));
        return false;
    }
    WsSendText(clientSock,JsonObj({JsonString("type","proactive_ok")}));
    ExitProcess(0);
}

static bool HandleNotPaid(SOCKET clientSock, const std::wstring& appDir) {
    if (!g_proactive){
        WsSendText(clientSock,JsonObj({JsonString("type","error"),JsonString("message","Not in proactive mode.")}));
        return false;
    }
    std::wstring originalExe=ReadProactiveMarker(appDir);
    if (originalExe.empty()){
        originalExe=GetExePath();
        size_t lastSep=originalExe.find_last_of(L"\\/");
        if (lastSep!=std::wstring::npos) originalExe=originalExe.substr(0,lastSep)+L"\\CipherLauncher.exe";
        else originalExe=L"CipherLauncher.exe";
    }
    DeleteFileW(Join(appDir,Cfg::PROACTIVE_MARKER).c_str());
    g_pool->CloseAll();
    if (!LaunchExeDetached(originalExe)){
        WsSendText(clientSock,JsonObj({JsonString("type","error"),JsonString("message","Could not restore normal mode.")}));
        return false;
    }
    WsSendText(clientSock,JsonObj({JsonString("type","notpaid_ok")}));
    ExitProcess(0);
}


// ══════════════════════════════════════════════════════════════════════════════
//  WebSocket client handler — one thread per connection, acquires engine from pool
// ══════════════════════════════════════════════════════════════════════════════
static void HandleClient(SOCKET clientSock, const std::wstring& appDir) {
    printf("[Server] client connected\n");

    if (!WsHandshake(clientSock)){
        printf("[Server] handshake failed\n");
        closesocket(clientSock);
        return;
    }

    // Send device fingerprint immediately on connect
    {
        std::string devData = GetWindowsDeviceData();
        if (!devData.empty())
            WsSendText(clientSock, JsonObj({JsonString("type","devicedata"),JsonString("data",devData)}));
    }

    while (true) {
        std::string payload;
        bool isClose=false;
        if (!WsReadFrame(clientSock,payload,isClose)){
            if (isClose) WsSendClose(clientSock);
            break;
        }

        std::string kind=JsonGetString(payload,"type");

        try {
            if (kind=="ping") {
                WsSendText(clientSock,JsonObj({JsonString("type","pong")}));

            } else if (kind=="ensure_nnue") {
                std::string nnue  =JsonGetString(payload,"nnue");
                std::string gdrive=JsonGetString(payload,"gdrive");
                if (!nnue.empty()&&!gdrive.empty())
                    EnsureNNUE(clientSock,appDir,nnue,gdrive);

            } else if (kind=="proactive") {
                HandleProactive(clientSock,appDir);

            } else if (kind=="notpaid") {
                HandleNotPaid(clientSock,appDir);

            } else if (kind=="configure") {
                // Acquire an engine, reconfigure it, release immediately.
                // This pre-warms the right variant in an idle engine.
                Engine* e = g_pool->Acquire(10000);
                if (!e) {
                    WsSendText(clientSock,JsonObj({JsonString("type","error"),
                        JsonString("message","All engines busy (configure timeout)")}));
                    continue;
                }
                std::string v=JsonGetString(payload,"variant");
                std::string n=JsonGetString(payload,"nnue");
                std::string g=JsonGetString(payload,"gdrive");
                if (v.empty()) v=e->variant;
                if (n.empty()) n=e->nnue;
                if (!n.empty()&&!g.empty()) EnsureNNUE(clientSock,appDir,n,g);
                e->Reconfigure(v,n);
                g_pool->Release(e);

            } else if (kind=="analyze") {
                // ── Acquire a free engine from the pool ─────────────────────
                Engine* e = g_pool->Acquire(30000);
                if (!e) {
                    WsSendText(clientSock,JsonObj({JsonString("type","error"),
                        JsonString("message","All engines busy — try again")}));
                    continue;
                }

                // Resolve variant and NNUE
                std::string v=JsonGetString(payload,"variant");
                std::string n=JsonGetString(payload,"nnue");
                std::string g=JsonGetString(payload,"gdrive");
                if (v.empty()) v=e->variant;
                if (n.empty()) n=e->nnue;

                // Ensure NNUE is on disk before we try to load it
                if (!n.empty()&&!g.empty()) {
                    if (!EnsureNNUE(clientSock,appDir,n,g)) {
                        g_pool->Release(e);
                        continue;
                    }
                }

                // Reconfigure only restarts the process when variant/nnue actually changed
                if (!e->Reconfigure(v,n)) {
                    WsSendText(clientSock,JsonObj({JsonString("type","error"),
                        JsonString("message","Engine reconfigure failed")}));
                    g_pool->Release(e);
                    continue;
                }

                auto moves   = JsonGetArray(payload,"moves");
                int movetime = JsonGetInt(payload,"movetime",Cfg::DEFAULT_MOVETIME);
                int elo      = JsonGetInt(payload,"elo",2200);
                // ── NEW: dual-axis Elo / movetime scaling ───────────────────
                if (elo <= 2850) {
                    // Strength-limited mode: use UCI_Elo handicap, fixed fast movetime
                    e->Send("setoption name UCI_LimitStrength value true");
                    e->Send("setoption name UCI_Elo value " + std::to_string(elo));
                    movetime = 1000;   // constant quick search for all handicap levels
                } else {
                    // Full strength mode: disable limit, scale movetime with rating
                    e->Send("setoption name UCI_LimitStrength value false");
                    // UCI_Elo is irrelevant when LimitStrength is false
                    const int minTime = 500;
                    const int maxTime = 3000;
                    movetime = minTime + (int)((elo - 2850) * (maxTime - minTime) / (3400.0 - 2850.0));
                }

                // Hard-restart engine if it died between jobs
                if (!e->IsAlive()) {
                    e->Close();
                    if (!e->Start(e->sfPathW, e->appDirW)) {
                        WsSendText(clientSock,JsonObj({JsonString("type","error"),
                            JsonString("message","Engine restart failed")}));
                        g_pool->Release(e);
                        continue;
                    }
                }

                std::string fromSq,toSq;
                bool ok=false;
                try {
                    ok = e->BestMove(moves,movetime,fromSq,toSq);
                } catch (const std::exception& ex) {
                    printf("[Engine %d] BestMove exception: %s\n", e->id, ex.what());
                    WsSendText(clientSock,JsonObj({JsonString("type","error"),
                        JsonString("message",ex.what())}));
                    g_pool->Release(e);
                    continue;
                }

                // ── Release engine back to the pool ─────────────────────────
                // Done before sending the response so the engine starts warming up
                // while the response is in flight.
                g_pool->Release(e);

                if (ok) {
                    WsSendText(clientSock,JsonObj({
                        JsonString("type","bestmove"),
                        JsonString("from",fromSq),
                        JsonString("to",toSq),
                        JsonString("move",fromSq+toSq),
                    }));
                } else {
                    WsSendText(clientSock,JsonObj({JsonString("type","error"),
                        JsonString("message","Engine returned no move")}));
                }
            }
        } catch (const std::exception& ex) {
            printf("[Server] unhandled exception: %s\n", ex.what());
        } catch (...) {
            printf("[Server] unknown exception\n");
        }
    }

    closesocket(clientSock);
    printf("[Server] client disconnected\n");
}


// ══════════════════════════════════════════════════════════════════════════════
//  WebSocket server loop
// ══════════════════════════════════════════════════════════════════════════════
static void RunServer(const std::wstring& appDir) {
    WSADATA wsa{}; WSAStartup(MAKEWORD(2,2),&wsa);
    SOCKET listenSock=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (listenSock==INVALID_SOCKET){LogFail(L"socket() failed");return;}
    int opt=1; setsockopt(listenSock,SOL_SOCKET,SO_REUSEADDR,(const char*)&opt,sizeof(opt));
    sockaddr_in addr{}; addr.sin_family=AF_INET;
    addr.sin_port=htons(Cfg::ENGINE_PORT); addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (bind(listenSock,(sockaddr*)&addr,sizeof(addr))!=0){
        LogFail(L"bind() failed on port 8765"); closesocket(listenSock); return;
    }
    listen(listenSock,32);
    LogOK(L"WebSocket server listening on ws://localhost:8765");
    printf("\n  Waiting for connections ...\n");
    while (true) {
        fd_set fds; FD_ZERO(&fds); FD_SET(listenSock,&fds);
        timeval tv{0,200000};
        if (select(0,&fds,nullptr,nullptr,&tv)<=0) continue;
        SOCKET clientSock=accept(listenSock,nullptr,nullptr);
        if (clientSock==INVALID_SOCKET) continue;
        std::thread([clientSock,appDir](){ HandleClient(clientSock,appDir); }).detach();
    }
    closesocket(listenSock); WSACleanup();
}


// ══════════════════════════════════════════════════════════════════════════════
//  Network helpers
// ══════════════════════════════════════════════════════════════════════════════
static bool IsPortListening(int port) {
    WSADATA wsa{}; WSAStartup(MAKEWORD(2,2),&wsa);
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    bool listening=false;
    if (s!=INVALID_SOCKET){
        DWORD timeout=400;
        setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&timeout,sizeof(timeout));
        setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const char*)&timeout,sizeof(timeout));
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons((u_short)port);
        a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        listening=(connect(s,(sockaddr*)&a,sizeof(a))==0);
        closesocket(s);
    }
    WSACleanup(); return listening;
}
static void KillProcessOnPort(int port) {
    auto kill=[](DWORD pid){ HANDLE h=OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE,FALSE,pid);
        if(h){TerminateProcess(h,0);WaitForSingleObject(h,3000);CloseHandle(h);} };
    { DWORD sz=0; GetExtendedTcpTable(nullptr,&sz,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0);
      std::vector<BYTE> buf(sz);
      if(GetExtendedTcpTable(buf.data(),&sz,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0)==NO_ERROR){
          auto* t=reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
          for(DWORD i=0;i<t->dwNumEntries;i++) if((int)ntohs((u_short)t->table[i].dwLocalPort)==port){kill(t->table[i].dwOwningPid);return;}
      }
    }
    { DWORD sz=0; GetExtendedTcpTable(nullptr,&sz,FALSE,AF_INET6,TCP_TABLE_OWNER_PID_LISTENER,0);
      std::vector<BYTE> buf(sz);
      if(GetExtendedTcpTable(buf.data(),&sz,FALSE,AF_INET6,TCP_TABLE_OWNER_PID_LISTENER,0)==NO_ERROR){
          auto* t=reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
          for(DWORD i=0;i<t->dwNumEntries;i++) if((int)ntohs((u_short)t->table[i].dwLocalPort)==port){kill(t->table[i].dwOwningPid);return;}
      }
    }
}
static HANDLE AcquireMutex() {
    HANDLE h=CreateMutexW(nullptr,TRUE,L"Global\\CipherEngineLauncherMutex_v3");
    if (GetLastError()==ERROR_ALREADY_EXISTS){if(h)CloseHandle(h);return nullptr;}
    return h;
}
static bool FilesIntact(const std::wstring& appDir) {
    bool ok=true;
    auto chk=[&](const std::wstring& p,DWORD minSz,const wchar_t* label){
        if(!FileReady(p,minSz)){wprintf(L"  [MISSING] %s\n",label);ok=false;}
    };
    chk(Join(appDir,Cfg::SF_EXE),   512*1024, L"fairy-stockfish.exe");
    chk(Join(appDir,Cfg::NNUE_FILE),1024*1024,L"NNUE weights");
    return ok;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Entry point
// ══════════════════════════════════════════════════════════════════════════════
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    std::wstring appDir     = GetAppDir();
    std::wstring markerPath = Join(appDir,Cfg::PROACTIVE_MARKER);
    if (FileExists(markerPath)) g_proactive = true;

    EnsureConsole();

    HANDLE mutex=AcquireMutex();
    if (!mutex){
        wprintf(L"\n  [%s] Another instance is already running.\n  Press Enter to close ...\n",
                g_proactive?L"Firefox":L"Cipher");
        getchar(); return 0;
    }

    const std::wstring sfPath   = Join(appDir,Cfg::SF_EXE);
    const std::wstring nnuePath = Join(appDir,Cfg::NNUE_FILE);
    const std::wstring destExe  = Join(appDir,Cfg::LAUNCHER_EXE);

    RemoveFromStartup();

    wprintf(L"\n");
    wprintf(L"  \x1b[1m\x1b[34m"
            L" ╔══════════════════════════════════════╗\n"
            L" ║       Cipher Engine Launcher v3.0     ║\n"
            L" ║    Multi-engine pool edition          ║\n"
            L" ╚══════════════════════════════════════╝"
            L"\x1b[0m\n\n");

    // ── Fast path ─────────────────────────────────────────────────────────────
    if (IsInstalled(appDir)&&FilesIntact(appDir)){
        Log(L"Installation detected — files OK");
        if (IsPortListening(Cfg::ENGINE_PORT)){
            Log(L"Port 8765 occupied — killing stale process ...");
            KillProcessOnPort(Cfg::ENGINE_PORT); Sleep(400);
        }
        goto run_engine;
    }

    // ── First-time setup ──────────────────────────────────────────────────────
    {
        wprintf(L"\n  \x1b[33m──────────────── First-time Setup ────────────────\x1b[0m\n\n");
        wprintf(L"  \x1b[33m[1/3]\x1b[0m Extracting Fairy-Stockfish ...\n");
        if (!FileReady(sfPath,512*1024)){
            if (!ExtractResource(IDR_STOCKFISH,sfPath,L"fairy-stockfish.exe")){
                LogFail(L"Cannot extract Fairy-Stockfish.");
                wprintf(L"\n  Press Enter to close ...\n");getchar();CloseHandle(mutex);return 1;
            }
        } else LogOK(L"fairy-stockfish.exe already present");

        wprintf(L"\n  \x1b[33m[2/3]\x1b[0m Extracting NNUE weights ...\n");
        if (!FileReady(nnuePath,1024*1024)){
            if (!ExtractAndDecompressNNUE(nnuePath))
                Log(L"NNUE extraction failed — engine will use default evaluation");
        } else LogOK(L"NNUE weights already present");

        wprintf(L"\n  \x1b[33m[3/3]\x1b[0m Install location\n");
        std::wstring selfPath=GetExePath();
        if (!FileExists(destExe)||selfPath!=destExe){
            if (CopyFileW(selfPath.c_str(),destExe.c_str(),FALSE)) LogOK(L"Launcher copied to AppDir");
            else LogFail(L"Could not copy launcher");
        }
        WriteMarker(appDir);
        if (IsPortListening(Cfg::ENGINE_PORT)){
            Log(L"Port 8765 occupied — killing stale process ...");
            KillProcessOnPort(Cfg::ENGINE_PORT); Sleep(400);
        }
        wprintf(L"\n  \x1b[1m\x1b[32m"
                L" ╔══════════════════════════════════════════════════╗\n"
                L" ║  Setup complete!                                  ║\n"
                L" ╚══════════════════════════════════════════════════╝"
                L"\x1b[0m\n\n");
    }

run_engine:
    // ── Auto-tune pool configuration for this machine ─────────────────────────
    MachineSpec  machine = QueryMachine();
    PoolConfig   poolCfg = ComputePoolConfig(machine);

    wprintf(L"\n");
    wprintf(L"  \x1b[36mMachine:\x1b[0m  %d logical cores  |  %zu MB RAM\n",
            machine.logicalCores, machine.totalRAM_MB);
    wprintf(L"  \x1b[36mPool:\x1b[0m     %d engine(s)  |  %d threads each  |  %d MB hash each\n\n",
            poolCfg.poolSize, poolCfg.threadsPerEngine, poolCfg.hashMB);

    EnginePool pool;
    g_pool = &pool;

    if (!pool.Init(poolCfg.poolSize, poolCfg.threadsPerEngine, poolCfg.hashMB, sfPath, appDir)) {
        LogFail(L"Failed to start engine pool.");
        wprintf(L"\n  Press Enter to close ...\n"); getchar();
        CloseHandle(mutex); return 1;
    }

    RunServer(appDir);

    pool.CloseAll();
    LogOK(L"Engine pool stopped");
    CloseHandle(mutex);
    return 0;
}