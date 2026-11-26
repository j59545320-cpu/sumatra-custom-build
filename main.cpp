#define NOMINMAX
#include <windows.h>
#include <winspool.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fpdfview.h"
#include "fpdf_sysfontinfo.h"

// ---------------- 本地私有字体加载与 PDFium 字体映射钩子 ----------------
typedef BOOL(WINAPI* PFN_GetFontResourceInfoW)(LPCWSTR, LPDWORD, LPVOID, DWORD);

static FPDF_SYSFONTINFO* g_origFontInfo = nullptr;
static FPDF_SYSFONTINFO  g_hookFontInfo = {};
static std::string       g_customFaceA;
static std::wstring      g_loadedFontPath;

static std::string WideToAnsi(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &out[0], len, nullptr, nullptr);
    return out;
}

static std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p(buf);
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

static void LoadLocalSongFont() {
    std::wstring dir = GetExeDir();
    const wchar_t* candidates[] = {
        L"SongStd-Light.otf",
        L"AdobeSongStd-Light.otf",
        L"STSong-Light.otf"
    };

    for (const wchar_t* name : candidates) {
        std::wstring fullPath = dir + L"\\" + name;
        if (GetFileAttributesW(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

        if (AddFontResourceExW(fullPath.c_str(), FR_PRIVATE, 0) > 0) {
            g_loadedFontPath = fullPath;

            HMODULE hGdi = GetModuleHandleW(L"gdi32.dll");
            auto pGetInfo = hGdi ? (PFN_GetFontResourceInfoW)GetProcAddress(hGdi, "GetFontResourceInfoW") : nullptr;
            if (pGetInfo) {
                DWORD cb = 0;
                if (pGetInfo(fullPath.c_str(), &cb, nullptr, 2 /* QFR_LOGFONT */) && cb >= sizeof(LOGFONTW)) {
                    std::vector<BYTE> raw(cb);
                    if (pGetInfo(fullPath.c_str(), &cb, raw.data(), 2)) {
                        const LOGFONTW* lf = reinterpret_cast<const LOGFONTW*>(raw.data());
                        g_customFaceA = WideToAnsi(lf->lfFaceName);
                    }
                }
            }
            if (g_customFaceA.empty()) g_customFaceA = "Adobe Song Std L";
            wprintf(L"[Font] Private loaded: %ls -> Face: %hs\n", name, g_customFaceA.c_str());
            return;
        }
    }
}

static void HookRelease(FPDF_SYSFONTINFO*) {}

static void HookEnumFonts(FPDF_SYSFONTINFO*, void* pMapper) {
    if (g_origFontInfo && g_origFontInfo->EnumFonts)
        g_origFontInfo->EnumFonts(g_origFontInfo, pMapper);
}

static void* HookMapFont(FPDF_SYSFONTINFO*, int weight, FPDF_BOOL bItalic, int charset,
                         int pitch_family, const char* face, FPDF_BOOL* bExact) {
    if (face && !g_customFaceA.empty() && g_origFontInfo && g_origFontInfo->MapFont) {
        std::string lower(face);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (lower.find("stsong") != std::string::npos ||
            lower.find("songstd") != std::string::npos ||
            lower.find("adobesong") != std::string::npos) {
            void* h = g_origFontInfo->MapFont(g_origFontInfo, weight, bItalic, charset,
                                              pitch_family, g_customFaceA.c_str(), bExact);
            if (h) return h;
        }
    }
    return (g_origFontInfo && g_origFontInfo->MapFont)
        ? g_origFontInfo->MapFont(g_origFontInfo, weight, bItalic, charset, pitch_family, face, bExact)
        : nullptr;
}

static void* HookGetFont(FPDF_SYSFONTINFO*, const char* face) {
    return (g_origFontInfo && g_origFontInfo->GetFont)
        ? g_origFontInfo->GetFont(g_origFontInfo, face) : nullptr;
}

static unsigned long HookGetFontData(FPDF_SYSFONTINFO*, void* hFont, unsigned int table,
                                     unsigned char* buffer, unsigned long buf_size) {
    return (g_origFontInfo && g_origFontInfo->GetFontData)
        ? g_origFontInfo->GetFontData(g_origFontInfo, hFont, table, buffer, buf_size) : 0;
}

static unsigned long HookGetFaceName(FPDF_SYSFONTINFO*, void* hFont,
                                     char* buffer, unsigned long buf_size) {
    return (g_origFontInfo && g_origFontInfo->GetFaceName)
        ? g_origFontInfo->GetFaceName(g_origFontInfo, hFont, buffer, buf_size) : 0;
}

static int HookGetFontCharset(FPDF_SYSFONTINFO*, void* hFont) {
    return (g_origFontInfo && g_origFontInfo->GetFontCharset)
        ? g_origFontInfo->GetFontCharset(g_origFontInfo, hFont) : 0;
}

static void HookDeleteFont(FPDF_SYSFONTINFO*, void* hFont) {
    if (g_origFontInfo && g_origFontInfo->DeleteFont)
        g_origFontInfo->DeleteFont(g_origFontInfo, hFont);
}

// ---------------- 工具函数：Unicode 读文件 & 获取默认打印机 ----------------
static bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(hFile, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 512LL * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD totalRead = 0;
    while (totalRead < (DWORD)sz.QuadPart) {
        DWORD chunk = 0;
        if (!ReadFile(hFile, out.data() + totalRead, (DWORD)sz.QuadPart - totalRead, &chunk, nullptr) || chunk == 0) {
            CloseHandle(hFile);
            return false;
        }
        totalRead += chunk;
    }
    CloseHandle(hFile);
    return true;
}

static std::wstring GetSystemDefaultPrinterName() {
    DWORD needed = 0;
    GetDefaultPrinterW(nullptr, &needed);
    if (needed == 0) return L"";
    std::vector<wchar_t> buf(needed);
    if (!GetDefaultPrinterW(buf.data(), &needed)) return L"";
    return std::wstring(buf.data());
}

static std::wstring ToLowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

// ---------------- 主入口：100% 兼容 SumatraPDF 命令行参数 ----------------
int wmain(int argc, wchar_t* argv[]) {
    std::wstring printerName;
    std::wstring pdfPath;
    short duplexMode = 0;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        std::wstring low = ToLowerW(arg);

        if (low == L"-silent") {
            continue;
        } else if (low == L"-print-to-default") {
            printerName = GetSystemDefaultPrinterName();
        } else if (low == L"-print-to" && i + 1 < argc) {
            printerName = argv[++i];
        } else if (low == L"-print-settings" && i + 1 < argc) {
            std::wstring s = ToLowerW(argv[++i]);
            if (s.find(L"duplexshort") != std::wstring::npos) {
                duplexMode = DMDUP_HORIZONTAL;
            } else if (s.find(L"duplexlong") != std::wstring::npos || s.find(L"duplex") != std::wstring::npos) {
                duplexMode = DMDUP_VERTICAL;
            } else if (s.find(L"simplex") != std::wstring::npos) {
                duplexMode = DMDUP_SIMPLEX;
            }
        } else if (!arg.empty() && arg[0] != L'-') {
            pdfPath = arg;
        }
    }

    if (pdfPath.empty()) {
        wprintf(L"Usage: PdfiumPrint.exe [-silent] (-print-to \"Printer\" | -print-to-default) [-print-settings \"simplex|duplexlong\"] \"file.pdf\"\n");
        return 1;
    }
    if (printerName.empty()) {
        printerName = GetSystemDefaultPrinterName();
    }
    if (printerName.empty()) {
        fwprintf(stderr, L"[ERROR] No default printer found.\n");
        return 2;
    }

    LoadLocalSongFont();

    FPDF_LIBRARY_CONFIG config = {};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);

    g_origFontInfo = FPDF_GetDefaultSystemFontInfo();
    if (g_origFontInfo) {
        g_hookFontInfo.version        = 1;
        g_hookFontInfo.Release        = HookRelease;
        g_hookFontInfo.EnumFonts      = HookEnumFonts;
        g_hookFontInfo.MapFont        = HookMapFont;
        g_hookFontInfo.GetFont        = HookGetFont;
        g_hookFontInfo.GetFontData    = HookGetFontData;
        g_hookFontInfo.GetFaceName    = HookGetFaceName;
        g_hookFontInfo.GetFontCharset = HookGetFontCharset;
        g_hookFontInfo.DeleteFont     = HookDeleteFont;
        FPDF_SetSystemFontInfo(&g_hookFontInfo);
    }

    std::vector<unsigned char> pdfBytes;
    if (!ReadWholeFile(pdfPath, pdfBytes)) {
        fwprintf(stderr, L"[ERROR] Cannot read PDF: %ls\n", pdfPath.c_str());
        return 3;
    }
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(pdfBytes.data(), (int)pdfBytes.size(), nullptr);
    if (!doc) {
        fwprintf(stderr, L"[ERROR] PDFium failed to open PDF (code=%lu).\n", FPDF_GetLastError());
        return 4;
    }

    HANDLE hPrinter = nullptr;
    std::vector<wchar_t> printerBuf(printerName.begin(), printerName.end());
    printerBuf.push_back(L'\0');

    if (!OpenPrinterW(printerBuf.data(), &hPrinter, nullptr)) {
        fwprintf(stderr, L"[ERROR] OpenPrinterW failed for: %ls\n", printerName.c_str());
        FPDF_CloseDocument(doc);
        return 5;
    }

    LONG dmSize = DocumentPropertiesW(nullptr, hPrinter, printerBuf.data(), nullptr, nullptr, 0);
    if (dmSize <= 0) {
        ClosePrinter(hPrinter);
        FPDF_CloseDocument(doc);
        return 6;
    }

    std::vector<BYTE> dmMem((size_t)dmSize, 0);
    DEVMODEW* dm = reinterpret_cast<DEVMODEW*>(dmMem.data());
    DocumentPropertiesW(nullptr, hPrinter, printerBuf.data(), dm, nullptr, DM_OUT_BUFFER);

    if (duplexMode != 0) {
        dm->dmFields |= DM_DUPLEX;
        dm->dmDuplex = duplexMode;
    }
    DocumentPropertiesW(nullptr, hPrinter, printerBuf.data(), dm, dm, DM_IN_BUFFER | DM_OUT_BUFFER);

    HDC hdc = CreateDCW(L"WINSPOOL", printerBuf.data(), nullptr, dm);
    ClosePrinter(hPrinter);
    if (!hdc) {
        fwprintf(stderr, L"[ERROR] CreateDCW failed.\n");
        FPDF_CloseDocument(doc);
        return 7;
    }

    size_t slash = pdfPath.find_last_of(L"\\/");
    std::wstring docTitle = (slash == std::wstring::npos) ? pdfPath : pdfPath.substr(slash + 1);

    DOCINFOW di = {};
    di.cbSize = sizeof(di);
    di.lpszDocName = docTitle.c_str();

    int pageCount = FPDF_GetPageCount(doc);
    wprintf(L"[Print] Printer: %ls | Duplex: %d | Pages: %d | File: %ls\n",
            printerName.c_str(), (int)duplexMode, pageCount, docTitle.c_str());

    if (StartDocW(hdc, &di) <= 0) {
        fwprintf(stderr, L"[ERROR] StartDocW failed.\n");
        DeleteDC(hdc);
        FPDF_CloseDocument(doc);
        return 8;
    }

    for (int i = 0; i < pageCount; ++i) {
        FPDF_PAGE page = FPDF_LoadPage(doc, i);
        if (!page) continue;

        double pageW_pt = FPDF_GetPageWidth(page);
        double pageH_pt = FPDF_GetPageHeight(page);

        bool pageLandscape = (pageW_pt > pageH_pt);
        bool dcLandscape = (GetDeviceCaps(hdc, PHYSICALWIDTH) > GetDeviceCaps(hdc, PHYSICALHEIGHT));
        if (pageLandscape != dcLandscape) {
            dm->dmFields |= DM_ORIENTATION;
            dm->dmOrientation = pageLandscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
            ResetDCW(hdc, dm);
        }

        int dpiX    = GetDeviceCaps(hdc, LOGPIXELSX);
        int dpiY    = GetDeviceCaps(hdc, LOGPIXELSY);
        int offsetX = GetDeviceCaps(hdc, PHYSICALOFFSETX);
        int offsetY = GetDeviceCaps(hdc, PHYSICALOFFSETY);

        int sizeX = (int)std::lround(pageW_pt * dpiX / 72.0);
        int sizeY = (int)std::lround(pageH_pt * dpiY / 72.0);

        if (StartPage(hdc) > 0) {
            FPDF_RenderPage(hdc, page, -offsetX, -offsetY, sizeX, sizeY, 0,
                            FPDF_PRINTING | FPDF_ANNOT);
            EndPage(hdc);
        }
        FPDF_ClosePage(page);
    }

    EndDoc(hdc);
    DeleteDC(hdc);
    FPDF_CloseDocument(doc);

    FPDF_SetSystemFontInfo(nullptr);
    FPDF_DestroyLibrary();
    if (g_origFontInfo) FPDF_FreeDefaultSystemFontInfo(g_origFontInfo);
    if (!g_loadedFontPath.empty()) RemoveFontResourceExW(g_loadedFontPath.c_str(), FR_PRIVATE, 0);

    return 0;
}
