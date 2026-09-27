/*
    KarnySnap - karnysnap.c

    Lekki rezydent Windows (7 SP1 / 8.1 / 10 / 11, x86 i x64):
    - przechwytuje globalnie klawisz Print Screen i blokuje jego domyslne dzialanie,
    - tryb AOI (domyslny): przyciemnia ekran, zaznaczony obszar pozostaje jasny,
      ESC lub ponowny Print Screen anuluje wybor,
    - tryb PELNY EKRAN: natychmiastowy zrzut calego ekranu,
    - zapis do PNG / JPG / BMP (domyslnie PNG) + kopia do schowka,
    - folder docelowy: %USERPROFILE%\Desktop\Screenshots (zmienialny z menu),
    - ikona w zasobniku systemowym, bez okna, bez konsoli, bez GUI,
    - ustawienia trwale w %LOCALAPPDATA%\KarnySnap\config.ini.

    Autor: KarnyJohnny
    Licencja: MIT
*/

#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>

#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Minimalny plaski interfejs GDI+ (bez naglowkow C++ gdiplus.h)     */
/* ------------------------------------------------------------------ */

typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

typedef void *GpImage;
typedef void *GpBitmap;

extern int __stdcall GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *input, void *output);
extern void __stdcall GdiplusShutdown(ULONG_PTR token);
extern int __stdcall GdipCreateBitmapFromScan0(int width, int height, int stride, int format, BYTE *scan0, GpBitmap *bitmap);
extern int __stdcall GdipSaveImageToFile(GpImage image, const WCHAR *filename, const CLSID *clsidEncoder, const void *encoderParams);
extern int __stdcall GdipDisposeImage(GpImage image);

#define PIXFMT_32BPPRGB 0x00022009

static const CLSID g_pngClsid  = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
static const CLSID g_jpegClsid = {0x557cf401, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

/* ------------------------------------------------------------------ */

#define MODE_FULL 0
#define MODE_AREA 1

#define FMT_PNG 0
#define FMT_JPG 1
#define FMT_BMP 2

#define WM_TRAYICON       (WM_APP + 1)
#define WM_DO_SCREENSHOT  (WM_APP + 2)
#define WM_DO_CANCEL_AOI  (WM_APP + 3)

#define IDM_MODE_FULL   40001
#define IDM_MODE_AREA   40002
#define IDM_FMT_PNG     40010
#define IDM_FMT_JPG     40011
#define IDM_FMT_BMP     40012
#define IDM_FOLDER_OPEN 40020
#define IDM_FOLDER_SET  40021
#define IDM_EXIT        40003

#define MAIN_CLASS            L"KarnySnapMain"
#define OVERLAY_CLASS         L"KarnySnapOverlay"
#define SINGLE_INSTANCE_NAME  L"Local\\KarnySnap_SingleInstance"
#define APP_TIP               L"KarnySnap"

#define PATH_BUF_LEN (MAX_PATH + 80)

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} BmpFileHeader;
#pragma pack(pop)

typedef struct {
    int w;
    int h;
    int pitch;
    uint8_t *bits;      /* bottom-up, 32bpp, wskazuje w HDC nizej */
    uint8_t *dimmed;     /* wlasny bufor: przyciemniona kopia (tylko tryb AOI) */
    HDC hdc;
    HBITMAP hbmp;
    HGDIOBJ old_bmp;
    BITMAPINFOHEADER bih;
} Capture;

static HINSTANCE g_hinst = NULL;
static HWND g_hwndMain = NULL;
static HWND g_hwndOverlay = NULL;
static HHOOK g_hook = NULL;
static NOTIFYICONDATAW g_nid = {0};

static int g_mode = MODE_AREA;      /* domyslnie AOI */
static int g_format = FMT_PNG;      /* domyslnie PNG */
static wchar_t g_shotDir[MAX_PATH] = L"Screenshots";
static wchar_t g_cfgPath[MAX_PATH] = L"";

static HANDLE g_mutex = NULL;

static bool g_capValid = false;
static Capture g_cap = {0};

static bool g_selecting = false;
static POINT g_ptStart = {0, 0};
static POINT g_ptEnd = {0, 0};

static bool g_busy = false;
static bool g_psDown = false;
static bool g_escDown = false;

static void show_selection_overlay(void);
static void close_overlay_if_open(void);
static void handle_screenshot_request(void);
static LRESULT CALLBACK OverlayWndProc(HWND, UINT, WPARAM, LPARAM);
static void save_config(void);

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int get_x(LPARAM lp) { return (int)(short)LOWORD(lp); }
static inline int get_y(LPARAM lp) { return (int)(short)HIWORD(lp); }

/* ------------------------------------------------------------------ */
/*  Konfiguracja (INI w %LOCALAPPDATA%\KarnySnap\config.ini)          */
/* ------------------------------------------------------------------ */

static void ensure_default_screenshot_dir(void)
{
    wchar_t desktop[MAX_PATH];

    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, desktop);

    if (SUCCEEDED(hr)) {
        int n = swprintf(g_shotDir, MAX_PATH, L"%ls\\Screenshots", desktop);
        if (n < 0) {
            wcscpy(g_shotDir, L"Screenshots");
        }
    } else {
        wcscpy(g_shotDir, L"Screenshots");
    }
}

static void ensure_dir_exists(const wchar_t *dir)
{
    if (!CreateDirectoryW(dir, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            /* ignorujemy - zapis do pliku i tak zglosi blad jesli potrzeba */
        }
    }
}

static void init_config_path(void)
{
    wchar_t base[MAX_PATH];

    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, base);

    if (SUCCEEDED(hr)) {
        swprintf(g_cfgPath, MAX_PATH, L"%ls\\KarnySnap", base);
        ensure_dir_exists(g_cfgPath);
        swprintf(g_cfgPath, MAX_PATH, L"%ls\\KarnySnap\\config.ini", base);
    } else {
        wcscpy(g_cfgPath, L"KarnySnap_config.ini");
    }
}

static void load_config(void)
{
    ensure_default_screenshot_dir();
    init_config_path();

    int mode = GetPrivateProfileIntW(L"General", L"Mode", MODE_AREA, g_cfgPath);
    int fmt  = GetPrivateProfileIntW(L"General", L"Format", FMT_PNG, g_cfgPath);

    g_mode = (mode == MODE_FULL) ? MODE_FULL : MODE_AREA;
    g_format = (fmt == FMT_JPG) ? FMT_JPG : (fmt == FMT_BMP) ? FMT_BMP : FMT_PNG;

    wchar_t folder[MAX_PATH];
    DWORD n = GetPrivateProfileStringW(L"General", L"Folder", L"", folder, MAX_PATH, g_cfgPath);

    if (n > 0) {
        wcsncpy(g_shotDir, folder, MAX_PATH - 1);
        g_shotDir[MAX_PATH - 1] = L'\0';
    }

    ensure_dir_exists(g_shotDir);
}

static void save_config(void)
{
    wchar_t buf[16];

    swprintf(buf, 16, L"%d", g_mode);
    WritePrivateProfileStringW(L"General", L"Mode", buf, g_cfgPath);

    swprintf(buf, 16, L"%d", g_format);
    WritePrivateProfileStringW(L"General", L"Format", buf, g_cfgPath);

    WritePrivateProfileStringW(L"General", L"Folder", g_shotDir, g_cfgPath);
}

/* ------------------------------------------------------------------ */
/*  Zapis / schowek                                                    */
/* ------------------------------------------------------------------ */

static bool copy_dib_to_clipboard(const BITMAPINFOHEADER *bih, const void *bits, DWORD bits_size)
{
    SIZE_T total = sizeof(BITMAPINFOHEADER) + (SIZE_T)bits_size;

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!h) {
        return false;
    }

    void *p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return false;
    }

    memcpy(p, bih, sizeof(*bih));
    memcpy((uint8_t *)p + sizeof(*bih), bits, bits_size);
    GlobalUnlock(h);

    bool opened = false;

    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(NULL)) {
            opened = true;
            break;
        }
        Sleep(10);
    }

    if (!opened) {
        GlobalFree(h);
        return false;
    }

    EmptyClipboard();
    HANDLE result = SetClipboardData(CF_DIB, h);
    CloseClipboard();

    if (!result) {
        GlobalFree(h);
        return false;
    }

    return true;
}

/* Zapis surowego BMP (bez GDI+, najlzejsza sciezka). */
static bool save_dib_as_bmp(const BITMAPINFOHEADER *bih, const void *bits, DWORD bits_size, const wchar_t *path)
{
    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    BmpFileHeader fh;
    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = (uint32_t)(sizeof(BmpFileHeader) + sizeof(BITMAPINFOHEADER));
    fh.bfSize = (uint32_t)(fh.bfOffBits + bits_size);

    DWORD written = 0;
    bool ok = WriteFile(hFile, &fh, sizeof(fh), &written, NULL) &&
              WriteFile(hFile, bih, sizeof(*bih), &written, NULL) &&
              WriteFile(hFile, bits, bits_size, &written, NULL);

    CloseHandle(hFile);
    return ok;
}

/* Zapis PNG/JPG przez GDI+. Dane wejsciowe: bottom-up 32bpp BGRX. */
static bool save_bits_as_image_gdiplus(int format, int w, int h, const uint8_t *bits, const wchar_t *path)
{
    ULONG_PTR token = 0;
    GdiplusStartupInput input;
    memset(&input, 0, sizeof(input));
    input.GdiplusVersion = 1;

    if (GdiplusStartup(&token, &input, NULL) != 0) {
        return false;
    }

    int stride = w * 4;
    BYTE *scan0 = (BYTE *)bits + (size_t)(h - 1) * (size_t)stride;

    GpBitmap gbmp = NULL;
    int status = GdipCreateBitmapFromScan0(w, h, -stride, PIXFMT_32BPPRGB, scan0, &gbmp);

    bool ok = false;

    if (status == 0 && gbmp) {
        const CLSID *clsid = (format == FMT_JPG) ? &g_jpegClsid : &g_pngClsid;
        ok = (GdipSaveImageToFile(gbmp, path, clsid, NULL) == 0);
        GdipDisposeImage(gbmp);
    }

    GdiplusShutdown(token);
    return ok;
}

static const wchar_t *ext_for_format(int format)
{
    switch (format) {
        case FMT_JPG: return L"jpg";
        case FMT_BMP: return L"bmp";
        default:      return L"png";
    }
}

static bool save_and_copy_dib(const BITMAPINFOHEADER *bih, const void *bits, DWORD bits_size)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t path[PATH_BUF_LEN];
    bool file_ok = false;

    for (int attempt = 0; attempt < 100 && !file_ok; ++attempt) {
        int n;

        if (attempt == 0) {
            n = swprintf(path, PATH_BUF_LEN, L"%ls\\Screenshot_%04d%02d%02d_%02d%02d%02d_%03d.%ls",
                         g_shotDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         st.wMilliseconds, ext_for_format(g_format));
        } else {
            n = swprintf(path, PATH_BUF_LEN, L"%ls\\Screenshot_%04d%02d%02d_%02d%02d%02d_%03d_%d.%ls",
                         g_shotDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         st.wMilliseconds, attempt, ext_for_format(g_format));
        }

        if (n < 0) {
            break;
        }

        DWORD attrs = GetFileAttributesW(path);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            continue; /* juz istnieje, sprobuj kolejnego numeru */
        }

        if (g_format == FMT_BMP) {
            file_ok = save_dib_as_bmp(bih, bits, bits_size, path);
        } else {
            file_ok = save_bits_as_image_gdiplus(g_format, bih->biWidth, bih->biHeight, (const uint8_t *)bits, path);
        }
    }

    bool clip_ok = copy_dib_to_clipboard(bih, bits, bits_size);

    return file_ok && clip_ok;
}

/* ------------------------------------------------------------------ */
/*  Przechwytywanie ekranu                                             */
/* ------------------------------------------------------------------ */

static void free_capture(Capture *cap)
{
    if (!cap) {
        return;
    }

    if (cap->hdc) {
        if (cap->old_bmp) {
            SelectObject(cap->hdc, cap->old_bmp);
        }
        if (cap->hbmp) {
            DeleteObject(cap->hbmp);
        }
        DeleteDC(cap->hdc);
    }

    if (cap->dimmed) {
        free(cap->dimmed);
    }

    memset(cap, 0, sizeof(*cap));
}

static bool capture_screen(Capture *cap)
{
    memset(cap, 0, sizeof(*cap));

    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int ox = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int oy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (w <= 0 || h <= 0) {
        return false;
    }

    HDC screen_dc = GetDC(NULL);
    if (!screen_dc) {
        return false;
    }

    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) {
        ReleaseDC(NULL, screen_dc);
        return false;
    }

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = h; /* dodatnie = bottom-up */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbmp = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);

    if (!hbmp) {
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return false;
    }

    HGDIOBJ old_bmp = SelectObject(mem_dc, hbmp);

    if (!BitBlt(mem_dc, 0, 0, w, h, screen_dc, ox, oy, SRCCOPY)) {
        SelectObject(mem_dc, old_bmp);
        DeleteObject(hbmp);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return false;
    }

    BITMAP bm;
    if (!GetObject(hbmp, sizeof(bm), &bm)) {
        SelectObject(mem_dc, old_bmp);
        DeleteObject(hbmp);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return false;
    }

    cap->w = w;
    cap->h = h;
    cap->pitch = bm.bmWidthBytes;
    cap->bits = (uint8_t *)bits;
    cap->hdc = mem_dc;
    cap->hbmp = hbmp;
    cap->old_bmp = old_bmp;
    cap->bih = bmi.bmiHeader;
    cap->bih.biSizeImage = (DWORD)cap->pitch * (DWORD)h;

    ReleaseDC(NULL, screen_dc);
    return true;
}

static void make_alpha_opaque(Capture *cap)
{
    if (!cap || !cap->bits) {
        return;
    }

    for (int y = 0; y < cap->h; ++y) {
        uint32_t *row = (uint32_t *)(cap->bits + (size_t)y * (size_t)cap->pitch);
        for (int x = 0; x < cap->w; ++x) {
            row[x] |= 0xFF000000u;
        }
    }
}

/* Buduje przyciemniona kopie (ok. 45% jasnosci) do podgladu AOI. */
static bool build_dimmed_copy(Capture *cap)
{
    size_t total = (size_t)cap->pitch * (size_t)cap->h;

    cap->dimmed = (uint8_t *)malloc(total);
    if (!cap->dimmed) {
        return false;
    }

    for (int y = 0; y < cap->h; ++y) {
        const uint32_t *src = (const uint32_t *)(cap->bits + (size_t)y * (size_t)cap->pitch);
        uint32_t *dst = (uint32_t *)(cap->dimmed + (size_t)y * (size_t)cap->pitch);

        for (int x = 0; x < cap->w; ++x) {
            uint32_t px = src[x];
            uint8_t b = (uint8_t)((px & 0xFF) * 45u / 100u);
            uint8_t g = (uint8_t)(((px >> 8) & 0xFF) * 45u / 100u);
            uint8_t r = (uint8_t)(((px >> 16) & 0xFF) * 45u / 100u);
            dst[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    return true;
}

static bool crop_to_dib(const Capture *cap, const RECT *r, BITMAPINFOHEADER *out_bih, void **out_bits, DWORD *out_size)
{
    int cw = r->right - r->left;
    int ch = r->bottom - r->top;

    if (cw <= 0 || ch <= 0) {
        return false;
    }

    DWORD pitch = (DWORD)cw * 4u;
    DWORD size = pitch * (DWORD)ch;

    uint8_t *bits = (uint8_t *)malloc(size);
    if (!bits) {
        return false;
    }

    for (int y = r->top; y < r->bottom; ++y) {
        int dest_row = ch - 1 - (y - r->top);

        const uint8_t *src = cap->bits + (size_t)(cap->h - 1 - y) * (size_t)cap->pitch + (size_t)r->left * 4u;
        uint8_t *dst = bits + (size_t)dest_row * (size_t)pitch;

        memcpy(dst, src, (size_t)cw * 4u);

        uint32_t *dst32 = (uint32_t *)dst;
        for (int x = 0; x < cw; ++x) {
            dst32[x] |= 0xFF000000u;
        }
    }

    memset(out_bih, 0, sizeof(*out_bih));
    out_bih->biSize = sizeof(BITMAPINFOHEADER);
    out_bih->biWidth = cw;
    out_bih->biHeight = ch;
    out_bih->biPlanes = 1;
    out_bih->biBitCount = 32;
    out_bih->biCompression = BI_RGB;
    out_bih->biSizeImage = size;

    *out_bits = bits;
    *out_size = size;
    return true;
}

static bool save_crop_rect(const Capture *cap, const RECT *r)
{
    BITMAPINFOHEADER bih;
    void *bits = NULL;
    DWORD size = 0;

    if (!crop_to_dib(cap, r, &bih, &bits, &size)) {
        return false;
    }

    bool ok = save_and_copy_dib(&bih, bits, size);
    free(bits);
    return ok;
}

static RECT normalized_sel_rect(void)
{
    RECT r;
    r.left = imin(g_ptStart.x, g_ptEnd.x);
    r.right = imax(g_ptStart.x, g_ptEnd.x);
    r.top = imin(g_ptStart.y, g_ptEnd.y);
    r.bottom = imax(g_ptStart.y, g_ptEnd.y);
    return r;
}

static POINT clamp_point(POINT p)
{
    if (!g_capValid) {
        return p;
    }

    if (p.x < 0) p.x = 0;
    if (p.y < 0) p.y = 0;
    if (p.x > g_cap.w) p.x = g_cap.w;
    if (p.y > g_cap.h) p.y = g_cap.h;

    return p;
}

/* Odswieza tylko obszar bedacy suma starego i nowego prostokata (+margines na ramke). */
static void invalidate_rect_union(HWND hwnd, const RECT *a, const RECT *b)
{
    RECT u;
    u.left = imin(a->left, b->left) - 2;
    u.top = imin(a->top, b->top) - 2;
    u.right = imax(a->right, b->right) + 2;
    u.bottom = imax(a->bottom, b->bottom) + 2;

    InvalidateRect(hwnd, &u, FALSE);
}

static void finish_selection(HWND hwnd, const RECT *r)
{
    if (!g_capValid) {
        g_hwndOverlay = NULL;
        DestroyWindow(hwnd);
        return;
    }

    g_busy = true;
    g_selecting = false;

    ShowWindow(hwnd, SW_HIDE);

    save_crop_rect(&g_cap, r);

    free_capture(&g_cap);
    g_capValid = false;

    g_hwndOverlay = NULL;
    DestroyWindow(hwnd);

    g_busy = false;
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            if (g_capValid && g_cap.dimmed) {
                BITMAPINFO bmi;
                memset(&bmi, 0, sizeof(bmi));
                bmi.bmiHeader = g_cap.bih;

                StretchDIBits(hdc, 0, 0, g_cap.w, g_cap.h, 0, 0, g_cap.w, g_cap.h,
                              g_cap.dimmed, &bmi, DIB_RGB_COLORS, SRCCOPY);

                if (g_selecting) {
                    RECT r = normalized_sel_rect();

                    if (r.right > r.left && r.bottom > r.top) {
                        BitBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                               g_cap.hdc, r.left, r.top, SRCCOPY);

                        HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 90, 60));
                        HGDIOBJ oldPen = SelectObject(hdc, pen);
                        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

                        Rectangle(hdc, r.left, r.top, r.right, r.bottom);

                        SelectObject(hdc, oldBrush);
                        SelectObject(hdc, oldPen);
                        DeleteObject(pen);
                    }
                }
            } else {
                FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (!g_capValid) {
                return 0;
            }

            POINT p;
            p.x = get_x(lParam);
            p.y = get_y(lParam);
            p = clamp_point(p);

            g_ptStart = p;
            g_ptEnd = p;
            g_selecting = true;

            SetCapture(hwnd);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!g_selecting || !g_capValid) {
                return 0;
            }

            RECT old_r = normalized_sel_rect();

            POINT p;
            p.x = get_x(lParam);
            p.y = get_y(lParam);
            p = clamp_point(p);
            g_ptEnd = p;

            RECT new_r = normalized_sel_rect();

            if (!EqualRect(&old_r, &new_r)) {
                invalidate_rect_union(hwnd, &old_r, &new_r);
                UpdateWindow(hwnd);
            }

            return 0;
        }

        case WM_LBUTTONUP: {
            if (!g_selecting) {
                return 0;
            }

            g_selecting = false;
            ReleaseCapture();

            RECT r = normalized_sel_rect();

            if (r.right <= r.left || r.bottom <= r.top) {
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            finish_selection(hwnd, &r);
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_ESCAPE) {
                PostMessageW(g_hwndMain, WM_DO_CANCEL_AOI, 0, 0);
            }
            return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            return 0;

        case WM_CLOSE:
            return 0;

        case WM_DESTROY:
            g_selecting = false;
            return 0;

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void show_selection_overlay(void)
{
    if (!g_capValid) {
        return;
    }

    if (!build_dimmed_copy(&g_cap)) {
        free_capture(&g_cap);
        g_capValid = false;
        return;
    }

    if (g_hwndOverlay) {
        DestroyWindow(g_hwndOverlay);
        g_hwndOverlay = NULL;
    }

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    g_hwndOverlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS,
        L"",
        WS_POPUP,
        vx, vy, g_cap.w, g_cap.h,
        NULL, NULL, g_hinst, NULL
    );

    if (!g_hwndOverlay) {
        free_capture(&g_cap);
        g_capValid = false;
        return;
    }

    ShowWindow(g_hwndOverlay, SW_SHOW);
    SetForegroundWindow(g_hwndOverlay);
    SetFocus(g_hwndOverlay);
    UpdateWindow(g_hwndOverlay);
}

static void close_overlay_if_open(void)
{
    if (g_hwndOverlay) {
        HWND h = g_hwndOverlay;
        g_hwndOverlay = NULL;
        g_selecting = false;

        ShowWindow(h, SW_HIDE);
        DestroyWindow(h);
    }

    if (g_capValid) {
        free_capture(&g_cap);
        g_capValid = false;
    }
}

static void handle_screenshot_request(void)
{
    if (g_busy) {
        return;
    }

    /* Ponowny Print Screen podczas AOI = anuluj (tak samo jak ESC). */
    if (g_hwndOverlay) {
        g_busy = true;
        close_overlay_if_open();
        g_busy = false;
        return;
    }

    g_busy = true;

    if (g_mode == MODE_FULL) {
        Capture cap;

        if (capture_screen(&cap)) {
            make_alpha_opaque(&cap);
            save_and_copy_dib(&cap.bih, cap.bits, cap.bih.biSizeImage);
            free_capture(&cap);
        }
    } else {
        if (capture_screen(&g_cap)) {
            g_capValid = true;
            g_selecting = false;
            g_ptStart.x = 0; g_ptStart.y = 0;
            g_ptEnd.x = 0; g_ptEnd.y = 0;

            show_selection_overlay();
        }
    }

    g_busy = false;
}

/* ------------------------------------------------------------------ */
/*  Hook klawiatury                                                    */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT *kb = (const KBDLLHOOKSTRUCT *)lParam;

        if (kb && kb->vkCode == VK_SNAPSHOT) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (!g_psDown) {
                    g_psDown = true;
                    if (g_hwndMain) {
                        PostMessageW(g_hwndMain, WM_DO_SCREENSHOT, 0, 0);
                    }
                }
                return 1;
            }

            if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                g_psDown = false;
                return 1;
            }
        }

        if (kb && kb->vkCode == VK_ESCAPE && g_hwndOverlay) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (!g_escDown) {
                    g_escDown = true;
                    PostMessageW(g_hwndMain, WM_DO_CANCEL_AOI, 0, 0);
                }
                return 1;
            }

            if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                g_escDown = false;
                return 1;
            }
        }
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  Menu zasobnika                                                     */
/* ------------------------------------------------------------------ */

static void pick_new_folder(HWND owner)
{
    wchar_t display[MAX_PATH] = L"";

    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = owner;
    bi.pszDisplayName = display;
    bi.lpszTitle = L"Wybierz folder zapisu zrzutow ekranu";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) {
        return;
    }

    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, path)) {
        wcsncpy(g_shotDir, path, MAX_PATH - 1);
        g_shotDir[MAX_PATH - 1] = L'\0';
        ensure_dir_exists(g_shotDir);
        save_config();
    }

    CoTaskMemFree(pidl);
}

static void show_tray_menu(HWND hwnd)
{
    POINT pt;
    if (!GetCursorPos(&pt)) {
        pt.x = 0;
        pt.y = 0;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, IDM_MODE_AREA, L"Zaznacz obszar (AOI)");
    AppendMenuW(menu, MF_STRING, IDM_MODE_FULL, L"Caly ekran");
    CheckMenuRadioItem(menu, IDM_MODE_AREA, IDM_MODE_FULL,
                        (g_mode == MODE_FULL) ? IDM_MODE_FULL : IDM_MODE_AREA, MF_BYCOMMAND);

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    HMENU fmtMenu = CreatePopupMenu();
    AppendMenuW(fmtMenu, MF_STRING, IDM_FMT_PNG, L"PNG");
    AppendMenuW(fmtMenu, MF_STRING, IDM_FMT_JPG, L"JPG");
    AppendMenuW(fmtMenu, MF_STRING, IDM_FMT_BMP, L"BMP");
    UINT fmtChecked = (g_format == FMT_JPG) ? IDM_FMT_JPG : (g_format == FMT_BMP) ? IDM_FMT_BMP : IDM_FMT_PNG;
    CheckMenuRadioItem(fmtMenu, IDM_FMT_PNG, IDM_FMT_BMP, fmtChecked, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)fmtMenu, L"Format obrazu");

    HMENU folderMenu = CreatePopupMenu();
    AppendMenuW(folderMenu, MF_STRING, IDM_FOLDER_OPEN, L"Otworz folder");
    AppendMenuW(folderMenu, MF_STRING, IDM_FOLDER_SET, L"Zmien folder...");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)folderMenu, L"Folder zapisu");

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Zamknij");

    SetForegroundWindow(hwnd);

    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);

    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void quit_app(void)
{
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }

    close_overlay_if_open();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    PostQuitMessage(0);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_TRAYICON:
            switch (lParam) {
                case WM_LBUTTONUP:
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    show_tray_menu(hwnd);
                    break;
                default:
                    break;
            }
            return 0;

        case WM_DO_SCREENSHOT:
            handle_screenshot_request();
            return 0;

        case WM_DO_CANCEL_AOI:
            if (g_hwndOverlay) {
                close_overlay_if_open();
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_MODE_FULL:
                    if (g_mode != MODE_FULL) {
                        g_mode = MODE_FULL;
                        close_overlay_if_open();
                        save_config();
                    }
                    break;

                case IDM_MODE_AREA:
                    if (g_mode != MODE_AREA) {
                        g_mode = MODE_AREA;
                        close_overlay_if_open();
                        save_config();
                    }
                    break;

                case IDM_FMT_PNG:
                    g_format = FMT_PNG;
                    save_config();
                    break;

                case IDM_FMT_JPG:
                    g_format = FMT_JPG;
                    save_config();
                    break;

                case IDM_FMT_BMP:
                    g_format = FMT_BMP;
                    save_config();
                    break;

                case IDM_FOLDER_OPEN:
                    ensure_dir_exists(g_shotDir);
                    ShellExecuteW(NULL, L"open", g_shotDir, NULL, NULL, SW_SHOWNORMAL);
                    break;

                case IDM_FOLDER_SET:
                    pick_new_folder(hwnd);
                    break;

                case IDM_EXIT:
                    quit_app();
                    break;

                default:
                    break;
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  Uruchomienie                                                       */
/* ------------------------------------------------------------------ */

static int run_app(HINSTANCE hInst)
{
    g_hinst = hInst;

    g_mutex = CreateMutexW(NULL, FALSE, SINGLE_INSTANCE_NAME);
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_mutex) {
            CloseHandle(g_mutex);
        }
        return 0;
    }

    SetProcessDPIAware();
    load_config();

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = MAIN_CLASS;

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        return 1;
    }

    WNDCLASSW ovc;
    memset(&ovc, 0, sizeof(ovc));
    ovc.lpfnWndProc = OverlayWndProc;
    ovc.hInstance = hInst;
    ovc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_CROSS);
    ovc.lpszClassName = OVERLAY_CLASS;

    if (!RegisterClassW(&ovc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        return 1;
    }

    g_hwndMain = CreateWindowExW(0, MAIN_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);

    if (!g_hwndMain) {
        CloseHandle(g_mutex);
        return 1;
    }

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwndMain;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;

    g_nid.hIcon = (HICON)LoadImageW(NULL, (LPCWSTR)IDI_INFORMATION, IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);

    if (!g_nid.hIcon) {
        g_nid.hIcon = (HICON)LoadImageW(NULL, (LPCWSTR)IDI_APPLICATION, IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    }

    wcsncpy(g_nid.szTip, APP_TIP, 127);
    g_nid.szTip[127] = L'\0';

    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        DestroyWindow(g_hwndMain);
        CloseHandle(g_mutex);
        MessageBoxW(NULL, L"Nie mozna dodac ikony zasobnika.", L"KarnySnap - blad", MB_ICONERROR);
        return 1;
    }

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(NULL), 0);

    if (!g_hook) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DestroyWindow(g_hwndMain);
        CloseHandle(g_mutex);
        MessageBoxW(NULL, L"Nie mozna zainstalowac hooka klawiatury.", L"KarnySnap - blad", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }

    close_overlay_if_open();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);

    if (g_hwndMain) {
        DestroyWindow(g_hwndMain);
        g_hwndMain = NULL;
    }

    if (g_mutex) {
        CloseHandle(g_mutex);
        g_mutex = NULL;
    }

    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    return run_app(hInstance);
}

/* Zapasowe wejscie na wypadek kompilacji bez -mwindows. */
int main(void)
{
    return run_app(GetModuleHandleW(NULL));
}
