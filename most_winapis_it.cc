// Windows SDK 10.0.22621.0+
#pragma execution_character_set("utf-8")
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <shellapi.h>
#include <strsafe.h>
#include <winerror.h>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <locale.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")

// Embed the manifest for proper Unicode and visual styles support
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Helper macro for error handling
#define RETURN_IF_FAILED(hr) do { HRESULT __hr = (hr); if (FAILED(__hr)) return __hr; } while(0)

constexpr UINT WM_WORK_DONE = WM_USER + 1;

// Global font handle for cleanup
static HFONT g_hFont = nullptr;

template <typename HandleType, typename Deleter>
struct auto_handle {
    HandleType h{};
    auto_handle() = default;
    explicit auto_handle(HandleType v) : h(v) {}
    ~auto_handle() { if (h) Deleter{}(h); }
    HandleType* operator&() { return &h; }
    HandleType get() const { return h; }
};

struct BCryptDeleter {
    void operator()(BCRYPT_ALG_HANDLE h) { BCryptCloseAlgorithmProvider(h, 0); }
};

struct HeapDeleter {
    void operator()(HANDLE h) { HeapDestroy(h); }
};

struct ThreadpoolWorkDeleter {
    void operator()(PTP_WORK w) { CloseThreadpoolWork(w); }
};

using auto_bcrypt_handle = auto_handle<BCRYPT_ALG_HANDLE, BCryptDeleter>;
using auto_heap_handle   = auto_handle<HANDLE, HeapDeleter>;
using auto_hp_work       = auto_handle<PTP_WORK, ThreadpoolWorkDeleter>;

struct WorkItem {
    std::wstring srcPath;
    HWND         hLbLog;
};

void Log(HWND hLb, PCWSTR fmt, ...) {
    WCHAR buf[512];
    va_list args;
    va_start(args, fmt);
    StringCbVPrintfW(buf, sizeof(buf), fmt, args);
    va_end(args);
    SendMessageW(hLb, LB_ADDSTRING, 0, (LPARAM)buf);
    SendMessageW(hLb, LB_SETTOPINDEX, SendMessage(hLb, LB_GETCOUNT, 0, 0) - 1, 0);
}

HRESULT Sha256File(HANDLE hFile, BYTE out[32]) {
    auto_bcrypt_handle hAlg;
    RETURN_IF_FAILED(BCryptOpenAlgorithmProvider(&hAlg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    BCRYPT_HASH_HANDLE hHash{};
    RETURN_IF_FAILED(BCryptCreateHash(hAlg.get(), &hHash, nullptr, 0, nullptr, 0, 0));
    BYTE buf[64 * 1024];
    DWORD read;
    while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read)
        BCryptHashData(hHash, buf, read, 0);
    RETURN_IF_FAILED(BCryptFinishHash(hHash, out, 32, 0));
    BCryptDestroyHash(hHash);
    return S_OK;
}

HRESULT AesEncryptFile(HANDLE hIn, HANDLE hOut, const BYTE key[32], const BYTE iv[16]) {
    auto_bcrypt_handle hAlg;
    RETURN_IF_FAILED(BCryptOpenAlgorithmProvider(&hAlg.h, BCRYPT_AES_ALGORITHM, nullptr, 0));
    RETURN_IF_FAILED(BCryptSetProperty(hAlg.get(), BCRYPT_CHAINING_MODE, (BYTE*)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0));
    BCRYPT_KEY_HANDLE hKey{};
    RETURN_IF_FAILED(BCryptGenerateSymmetricKey(hAlg.get(), &hKey, nullptr, 0, (BYTE*)key, 32, 0));
    BYTE buf[64 * 1024];
    DWORD read, written;
    BOOL finish = FALSE;
    while (!finish) {
        if (!ReadFile(hIn, buf, sizeof(buf), &read, nullptr)) break;
        finish = (read < sizeof(buf));
        DWORD cipherLen = read + 16; // padding
        BYTE cipher[64 * 1024 + 16];
        RETURN_IF_FAILED(BCryptEncrypt(hKey, buf, read, nullptr, (BYTE*)iv, 16, cipher, cipherLen, &cipherLen, finish ? BCRYPT_BLOCK_PADDING : 0));
        WriteFile(hOut, cipher, cipherLen, &written, nullptr);
    }
    BCryptDestroyKey(hKey);
    return S_OK;
}

DWORD WINAPI UploadJson(PCWSTR json) {
    HINTERNET hSess = WinHttpOpen(L"OneBox/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConn = WinHttpConnect(hSess, L"api.example.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hReq  = WinHttpOpenRequest(hConn, L"POST", L"/backup", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    BOOL ok = WinHttpSendRequest(hReq, L"Content-Type: application/json\r\n", -1, (LPVOID)json, DWORD(wcslen(json) * 2), DWORD(wcslen(json) * 2), 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
    return ok ? NOERROR : ERROR_WINHTTP_TIMEOUT;
}

VOID CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WORK) {
    std::unique_ptr<WorkItem> item(static_cast<WorkItem*>(ctx));
    Log(item->hLbLog, L"[ThreadPool] 开始处理 %s", item->srcPath.c_str());

    HANDLE hIn = CreateFileW(item->srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hIn == INVALID_HANDLE_VALUE) return;
    BYTE hash[32];
    Sha256File(hIn, hash);
    Log(item->hLbLog, L"SHA-256 = %02X%02X%02X...", hash[0], hash[1], hash[2]);

    WCHAR aesPath[MAX_PATH];
    StringCbPrintfW(aesPath, sizeof(aesPath), L"%s.aes", item->srcPath.c_str());
    HANDLE hOut = CreateFileW(aesPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    BYTE key[32], iv[16];
    BCryptGenRandom(nullptr, key, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    BCryptGenRandom(nullptr, iv, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    AesEncryptFile(hIn, hOut, key, iv);
    CloseHandle(hIn); CloseHandle(hOut);

    WCHAR json[1024];
    StringCbPrintfW(json, sizeof(json), L"{\"file\":\"%s\",\"sha256\":\"%02X%02X...\",\"keyEnc\":\"RSA_ENC_DATA\"}", item->srcPath.c_str(), hash[0], hash[1]);
    UploadJson(json);

    PostMessage(GetParent(item->hLbLog), WM_WORK_DONE, 0, 0);
}

void OnDropFiles(HWND hWnd, HDROP hDrop) {
    UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        WCHAR path[MAX_PATH];
        DragQueryFileW(hDrop, i, path, MAX_PATH);
        auto* item = new WorkItem{ path, GetDlgItem(hWnd, 103) };
        auto_hp_work work(CreateThreadpoolWork(WorkCallback, item, nullptr));
        SubmitThreadpoolWork(work.get());
    }
    DragFinish(hDrop);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        DragAcceptFiles(hWnd, TRUE);
        break;
    case WM_DROPFILES:
        OnDropFiles(hWnd, (HDROP)wp);
        break;
    case WM_WORK_DONE:
        Log(GetDlgItem(hWnd, 103), L"=== 后台任务完成 ===");
        break;
    case WM_DESTROY:
        // Clean up the font if it was created
        if (g_hFont && g_hFont != (HFONT)GetStockObject(SYSTEM_FONT)) {
            DeleteObject(g_hFont);
        }
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, msg, wp, lp);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmd) {
    // Set locale to support Chinese characters
    setlocale(LC_ALL, "");
    
    // Ensure UTF-8 code page for proper Unicode handling
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    
    INITCOMMONCONTROLSEX ic{ sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"OneBox";
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(0, L"OneBox", L"OneBox – 拖进来就加密上传",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, 300, 400,
        nullptr, nullptr, hInst, nullptr);

    HWND hListBox = CreateWindowW(L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
        0, 0, 300, 400, hWnd, (HMENU)103, hInst, nullptr);
    
    // Create a font that properly supports Chinese characters
    HFONT hFont = CreateFontW(
        -14,                        // Height
        0,                          // Width
        0,                          // Escapement
        0,                          // Orientation
        FW_NORMAL,                  // Weight
        FALSE,                      // Italic
        FALSE,                      // Underline
        FALSE,                      // StrikeOut
        GB2312_CHARSET,             // CharSet - specifically for Chinese
        OUT_DEFAULT_PRECIS,         // OutPrecision
        CLIP_DEFAULT_PRECIS,        // ClipPrecision
        CLEARTYPE_QUALITY,          // Quality - better rendering
        DEFAULT_PITCH | FF_DONTCARE, // PitchAndFamily
        L"Microsoft YaHei"          // Face name - good Chinese support
    );
    
    // Fallback to SimSun if Microsoft YaHei is not available
    if (!hFont) {
        hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"SimSun");
    }
    
    // Final fallback to system font
    if (!hFont) {
        hFont = (HFONT)GetStockObject(SYSTEM_FONT);
    }
    
    // Store globally for cleanup and apply to listbox
    g_hFont = hFont;
    if (hFont) {
        SendMessage(hListBox, WM_SETFONT, (WPARAM)hFont, TRUE);
    }

    ShowWindow(hWnd, nCmd);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}