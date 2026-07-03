#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <stdint.h>

#include "resource.h"

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:ShowDualBoxBlockedDialog=_ShowDualBoxBlockedDialog@16")
#endif

#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif

namespace {

const wchar_t kWindowClassName[] = L"NexoraVanguardWindow";
const wchar_t kMenuClassName[] = L"NexoraVanguardMenuWindow";
const wchar_t kDialogClassName[] = L"NexoraVanguardDialogWindow";
const wchar_t kWindowTitle[] = L"Nexora Vanguard";
const wchar_t kInstanceMutexName[] = L"Local\\NexoraVanguardTrayInstance";
const wchar_t kClientSlotsGlobalPrefix[] = L"Global\\NexoraVanguardAionClientSlots_";
const wchar_t kClientSlotsLocalPrefix[] = L"Local\\NexoraVanguardAionClientSlots_";
const wchar_t kDualBoxDialogEntry[] = L"ShowDualBoxBlockedDialog";
const UINT kTrayMessage = WM_APP + 45;
const UINT kShutdownMessage = WM_APP + 46;
const UINT_PTR kTrayId = 1;

const int kWindowWidth = 300;
const int kWindowHeight = 382;
const int kHeaderHeight = 38;
const int kRowCount = 6;
const int kMenuWidth = 246;
const int kMenuPadding = 8;
const int kMenuItemHeight = 32;
const int kMenuSeparatorHeight = 9;
const int kMaxMenuItems = 12;
const int kDialogWidth = 420;
const int kDialogMinHeight = 226;
const int kDialogMaxHeight = 380;
const int kDialogHeaderHeight = 42;
const int kDialogButtonHeight = 34;

const int IDM_OPEN = 1001;
const int IDM_SAVE_LOGS = 1002;
const int IDM_PRECHECK = 1003;
const int IDM_TOGGLE_THEME = 1004;
const int IDM_EXIT = 1005;
const int IDM_LANG_PTBR = 1101;
const int IDM_ADVANCED = 1201;

enum StatusKind {
    StatusOk,
    StatusWarning,
    StatusBad
};

enum ThemeKind {
    ThemeLight,
    ThemeDark
};

enum DialogKind {
    DialogSecurity,
    DialogWarning,
    DialogError,
    DialogSuccess,
    DialogInformation
};

struct Requirement {
    const wchar_t* title;
    const wchar_t* detail;
    StatusKind status;
    RECT rect;
};

struct Palette {
    COLORREF frame;
    COLORREF header;
    COLORREF surface;
    COLORREF row;
    COLORREF rowHover;
    COLORREF border;
    COLORREF text;
    COLORREF muted;
    COLORREF disabledText;
    COLORREF button;
    COLORREF buttonBorder;
    COLORREF tooltip;
    COLORREF red;
    COLORREF yellow;
    COLORREF green;
    COLORREF blue;
    COLORREF accentText;
};

struct MenuItem {
    const wchar_t* text;
    UINT command;
    bool enabled;
    bool separator;
    bool checked;
    RECT rect;
};

struct VanguardDialogConfig {
    DialogKind kind;
    const wchar_t* title;
    const wchar_t* primaryText;
    const wchar_t* secondaryText;
    const wchar_t* detailText;
    const wchar_t* primaryButton;
    const wchar_t* secondaryButton;
    bool topMost;
};

struct VanguardDialogState {
    VanguardDialogConfig config;
    int result;
    int width;
    int height;
    bool done;
    bool closeHover;
    bool primaryHover;
    bool secondaryHover;
    RECT closeRect;
    RECT primaryButtonRect;
    RECT secondaryButtonRect;
};

struct RtlOsVersionInfoW {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    WCHAR szCSDVersion[128];
};

typedef LONG(WINAPI* RtlGetVersionFn)(RtlOsVersionInfoW*);
typedef UINT32(WINAPI* TbsiGetDeviceInfoFn)(UINT32, void*);

struct TbsDeviceInfo {
    UINT32 structVersion;
    UINT32 tpmVersion;
    UINT32 tpmInterfaceType;
    UINT32 tpmImpRevision;
};

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HWND g_menuWindow = nullptr;
HWND g_menuOwner = nullptr;
NOTIFYICONDATAW g_tray = {};
HICON g_iconSmall = nullptr;
HICON g_iconLarge = nullptr;
HANDLE g_instanceMutex = nullptr;
HANDLE g_clientSlots = nullptr;
DWORD g_uiThreadId = 0;
ThemeKind g_theme = ThemeLight;
Requirement g_requirements[kRowCount] = {};
MenuItem g_menuItems[kMaxMenuItems] = {};
int g_hoverRow = -1;
int g_menuHover = -1;
int g_menuItemCount = 0;
int g_menuHeight = 0;
bool g_hoverFooter = false;
bool g_trackingMouse = false;
bool g_clientSlotAcquired = false;
volatile LONG g_started = 0;
volatile LONG g_stopping = 0;

void StartNexoraVanguard(HINSTANCE instance);

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect = { left, top, right, bottom };
    return rect;
}

bool ContainsPoint(const RECT& rect, int x, int y) {
    POINT point = { x, y };
    return PtInRect(&rect, point) != FALSE;
}

const wchar_t* FileNamePart(const wchar_t* path) {
    const wchar_t* fileName = path;
    for (const wchar_t* scan = path; scan && *scan; ++scan) {
        if (*scan == L'\\' || *scan == L'/') {
            fileName = scan + 1;
        }
    }
    return fileName;
}

bool GetCurrentProcessFileName(wchar_t* buffer, size_t bufferCount) {
    if (!buffer || bufferCount == 0) {
        return false;
    }

    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) {
        return false;
    }

    StringCchCopyW(buffer, bufferCount, FileNamePart(path));
    return true;
}

bool IsRundll32HostProcess() {
    wchar_t fileName[MAX_PATH] = {};
    if (!GetCurrentProcessFileName(fileName, ARRAYSIZE(fileName))) {
        return false;
    }

    return lstrcmpiW(fileName, L"rundll32.exe") == 0;
}

void NormalizeClientIdentity(const wchar_t* input, wchar_t* output, size_t outputCount) {
    if (!output || outputCount == 0) {
        return;
    }

    output[0] = L'\0';
    if (!input || !*input) {
        StringCchCopyW(output, outputCount, L"unknown_client");
        return;
    }

    size_t write = 0;
    for (size_t read = 0; input[read] != L'\0' && write + 1 < outputCount; ++read) {
        wchar_t ch = input[read];
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }

        bool alpha = ch >= L'a' && ch <= L'z';
        bool digit = ch >= L'0' && ch <= L'9';
        if (alpha || digit) {
            output[write++] = ch;
        } else if (write > 0 && output[write - 1] != L'_') {
            output[write++] = L'_';
        }
    }

    while (write > 0 && output[write - 1] == L'_') {
        --write;
    }

    output[write] = L'\0';
    if (write == 0) {
        StringCchCopyW(output, outputCount, L"unknown_client");
    }
}

bool GetCurrentClientIdentity(wchar_t* identity, size_t identityCount) {
    wchar_t fileName[MAX_PATH] = {};
    if (!GetCurrentProcessFileName(fileName, ARRAYSIZE(fileName))) {
        NormalizeClientIdentity(L"unknown_client", identity, identityCount);
        return false;
    }

    NormalizeClientIdentity(fileName, identity, identityCount);
    return true;
}

bool BuildClientSlotName(const wchar_t* prefix, wchar_t* buffer, size_t bufferCount) {
    wchar_t identity[80] = {};
    GetCurrentClientIdentity(identity, ARRAYSIZE(identity));
    return SUCCEEDED(StringCchPrintfW(buffer, bufferCount, L"%s%s", prefix, identity));
}

DWORD GetParentProcessId(DWORD processId) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD parent = 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == processId) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return parent;
}

bool IsProcessAncestor(DWORD ancestor, DWORD child) {
    if (ancestor == 0 || child == 0 || ancestor == child) {
        return false;
    }

    DWORD cursor = child;
    for (int depth = 0; depth < 8; ++depth) {
        DWORD parent = GetParentProcessId(cursor);
        if (parent == 0 || parent == cursor) {
            return false;
        }
        if (parent == ancestor) {
            return true;
        }
        cursor = parent;
    }

    return false;
}

bool IsSameLaunchLineage(DWORD otherProcessId, DWORD currentProcessId) {
    return IsProcessAncestor(otherProcessId, currentProcessId) ||
        IsProcessAncestor(currentProcessId, otherProcessId);
}

bool IsInternalClientTransitionInProgress(const wchar_t* identity) {
    if (!identity || !*identity) {
        return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD currentProcessId = GetCurrentProcessId();
    bool foundLineageClient = false;
    bool foundSeparateClient = false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == currentProcessId) {
                continue;
            }

            wchar_t otherIdentity[80] = {};
            NormalizeClientIdentity(entry.szExeFile, otherIdentity, ARRAYSIZE(otherIdentity));
            if (lstrcmpiW(identity, otherIdentity) != 0) {
                continue;
            }

            if (IsSameLaunchLineage(entry.th32ProcessID, currentProcessId)) {
                foundLineageClient = true;
            } else {
                foundSeparateClient = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return foundLineageClient && !foundSeparateClient;
}

bool HasSeparateClientProcess(const wchar_t* identity) {
    if (!identity || !*identity) {
        return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return true;
    }

    DWORD currentProcessId = GetCurrentProcessId();
    bool foundSeparateClient = false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == currentProcessId) {
                continue;
            }

            wchar_t otherIdentity[80] = {};
            NormalizeClientIdentity(entry.szExeFile, otherIdentity, ARRAYSIZE(otherIdentity));
            if (lstrcmpiW(identity, otherIdentity) != 0) {
                continue;
            }

            if (!IsSameLaunchLineage(entry.th32ProcessID, currentProcessId)) {
                foundSeparateClient = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return foundSeparateClient;
}

bool IsSecondClientAttempt() {
    wchar_t identity[80] = {};
    GetCurrentClientIdentity(identity, ARRAYSIZE(identity));
    return HasSeparateClientProcess(identity);
}

int InternalMaxGameClients() {
    // Internal build-time limit. Do not expose this through config, registry, or UI.
    return 0x5A ^ 0x5B;
}

DWORD WINAPI DeferredGameClientSlotThread(LPVOID parameter) {
    HANDLE semaphore = reinterpret_cast<HANDLE>(parameter);
    if (!semaphore) {
        return 0;
    }

    if (WaitForSingleObject(semaphore, INFINITE) == WAIT_OBJECT_0) {
        g_clientSlotAcquired = true;
        for (int attempt = 0; attempt < 20 && InterlockedCompareExchange(&g_started, 0, 0) == 0; ++attempt) {
            StartNexoraVanguard(g_instance);
            if (InterlockedCompareExchange(&g_started, 0, 0) != 0) {
                break;
            }
            Sleep(250);
        }
    }

    return 0;
}

bool AcquireGameClientSlot() {
    if (g_clientSlotAcquired) {
        return true;
    }

    int maxClients = InternalMaxGameClients();
    if (maxClients < 1) {
        maxClients = 1;
    }

    wchar_t globalName[160] = {};
    wchar_t localName[160] = {};
    BuildClientSlotName(kClientSlotsGlobalPrefix, globalName, ARRAYSIZE(globalName));
    BuildClientSlotName(kClientSlotsLocalPrefix, localName, ARRAYSIZE(localName));

    const wchar_t* names[] = {
        globalName,
        localName
    };

    for (int i = 0; i < ARRAYSIZE(names); ++i) {
        HANDLE semaphore = CreateSemaphoreW(nullptr, maxClients, maxClients, names[i]);
        if (!semaphore) {
            continue;
        }

        DWORD wait = WaitForSingleObject(semaphore, 0);
        if (wait == WAIT_OBJECT_0) {
            g_clientSlots = semaphore;
            g_clientSlotAcquired = true;
            return true;
        }

        wchar_t identity[80] = {};
        GetCurrentClientIdentity(identity, ARRAYSIZE(identity));
        if (IsInternalClientTransitionInProgress(identity)) {
            g_clientSlots = semaphore;
            HANDLE thread = CreateThread(nullptr, 0, DeferredGameClientSlotThread, semaphore, 0, nullptr);
            if (thread) {
                CloseHandle(thread);
            }
            return true;
        }

        CloseHandle(semaphore);
        return false;
    }

    return false;
}

bool CanStartGameClient() {
    return AcquireGameClientSlot();
}

void ReleaseGameClientSlot() {
    if (g_clientSlotAcquired && g_clientSlots) {
        ReleaseSemaphore(g_clientSlots, 1, nullptr);
    }

    if (g_clientSlots) {
        CloseHandle(g_clientSlots);
        g_clientSlots = nullptr;
    }

    g_clientSlotAcquired = false;
}

void ShowDualBoxBlockedDialogProcess() {
    wchar_t dllPath[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_instance, dllPath, ARRAYSIZE(dllPath))) {
        return;
    }

    wchar_t rundllPath[MAX_PATH] = {};
    UINT systemLength = GetSystemDirectoryW(rundllPath, ARRAYSIZE(rundllPath));
    if (systemLength == 0 || systemLength >= ARRAYSIZE(rundllPath)) {
        return;
    }
    StringCchCatW(rundllPath, ARRAYSIZE(rundllPath), L"\\rundll32.exe");

    wchar_t commandLine[MAX_PATH * 2 + 96] = {};
    if (FAILED(StringCchPrintfW(
            commandLine,
            ARRAYSIZE(commandLine),
            L"\"%s\" \"%s\",%s",
            rundllPath,
            dllPath,
            kDualBoxDialogEntry))) {
        return;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (CreateProcessW(
            nullptr,
            commandLine,
            nullptr,
            nullptr,
            FALSE,
            CREATE_DEFAULT_ERROR_MODE,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void BlockExtraGameClient() {
    ShowDualBoxBlockedDialogProcess();
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);
}

Palette GetPalette() {
    Palette p = {};
    if (g_theme == ThemeDark) {
        p.frame = RGB(8, 9, 11);
        p.header = RGB(16, 17, 19);
        p.surface = RGB(12, 13, 15);
        p.row = RGB(18, 19, 21);
        p.rowHover = RGB(28, 30, 34);
        p.border = RGB(37, 40, 46);
        p.text = RGB(241, 243, 246);
        p.muted = RGB(137, 145, 157);
        p.disabledText = RGB(88, 93, 101);
        p.button = RGB(17, 18, 20);
        p.buttonBorder = RGB(34, 37, 42);
        p.tooltip = RGB(23, 25, 29);
    } else {
        p.frame = RGB(234, 238, 244);
        p.header = RGB(250, 252, 255);
        p.surface = RGB(232, 237, 244);
        p.row = RGB(247, 249, 252);
        p.rowHover = RGB(219, 225, 234);
        p.border = RGB(207, 216, 228);
        p.text = RGB(24, 30, 39);
        p.muted = RGB(105, 116, 130);
        p.disabledText = RGB(112, 122, 136);
        p.button = RGB(241, 245, 249);
        p.buttonBorder = RGB(207, 216, 228);
        p.tooltip = RGB(247, 250, 253);
    }

    p.red = RGB(255, 69, 91);
    p.yellow = RGB(255, 190, 44);
    p.green = RGB(45, 204, 122);
    p.blue = RGB(74, 126, 245);
    p.accentText = RGB(255, 255, 255);
    return p;
}

HFONT CreateUiFont(HDC hdc, int points, int weight) {
    LOGFONTW font = {};
    font.lfHeight = -MulDiv(points, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    font.lfWeight = weight;
    font.lfQuality = CLEARTYPE_QUALITY;
    StringCchCopyW(font.lfFaceName, ARRAYSIZE(font.lfFaceName), L"Segoe UI");
    return CreateFontIndirectW(&font);
}

void FillSolid(HDC hdc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

void FillRound(HDC hdc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawTextLine(HDC hdc, const RECT& rect, const wchar_t* text, HFONT font, COLORREF color, UINT format) {
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    RECT textRect = rect;
    DrawTextW(hdc, text, -1, &textRect, format);
    SelectObject(hdc, oldFont);
}

void DrawStatusMark(HDC hdc, const RECT& rect, StatusKind status, const Palette& p) {
    int cx = (rect.left + rect.right) / 2;
    int cy = (rect.top + rect.bottom) / 2;

    if (status == StatusOk) {
        HPEN pen = CreatePen(PS_SOLID, 2, p.green);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, cx - 5, cy, nullptr);
        LineTo(hdc, cx - 1, cy + 4);
        LineTo(hdc, cx + 6, cy - 5);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        return;
    }

    if (status == StatusWarning) {
        HPEN pen = CreatePen(PS_SOLID, 1, p.yellow);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        POINT points[3] = {
            { cx, cy - 7 },
            { cx - 7, cy + 6 },
            { cx + 7, cy + 6 }
        };
        Polyline(hdc, points, 3);
        LineTo(hdc, cx, cy - 7);
        MoveToEx(hdc, cx, cy - 2, nullptr);
        LineTo(hdc, cx, cy + 2);
        SetPixel(hdc, cx, cy + 5, p.yellow);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        return;
    }

    HPEN pen = CreatePen(PS_SOLID, 2, p.red);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, cx - 4, cy - 4, nullptr);
    LineTo(hdc, cx + 5, cy + 5);
    MoveToEx(hdc, cx + 4, cy - 4, nullptr);
    LineTo(hdc, cx - 5, cy + 5);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

LRESULT CALLBACK VanguardDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

bool HasText(const wchar_t* text) {
    return text && *text;
}

int ClampInt(int value, int minValue, int maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

COLORREF DialogAccent(DialogKind kind, const Palette& p) {
    if (kind == DialogSecurity || kind == DialogError) {
        return p.red;
    }
    if (kind == DialogWarning) {
        return p.yellow;
    }
    if (kind == DialogSuccess) {
        return p.green;
    }
    return p.blue;
}

int MeasureDialogTextHeight(const wchar_t* text, int width, int points, int weight) {
    if (!HasText(text)) {
        return 0;
    }

    HDC hdc = GetDC(nullptr);
    if (!hdc) {
        return 0;
    }

    HFONT font = CreateUiFont(hdc, points, weight);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    RECT measure = MakeRect(0, 0, width, 0);
    DrawTextW(hdc, text, -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
    ReleaseDC(nullptr, hdc);

    return measure.bottom - measure.top;
}

int CalculateDialogHeight(const VanguardDialogConfig& config) {
    const int textWidth = kDialogWidth - 116;
    int textHeight = MeasureDialogTextHeight(config.primaryText, textWidth, 13, FW_BOLD);
    if (HasText(config.secondaryText)) {
        textHeight += 9 + MeasureDialogTextHeight(config.secondaryText, textWidth, 9, FW_NORMAL);
    }
    if (HasText(config.detailText)) {
        textHeight += 10 + MeasureDialogTextHeight(config.detailText, textWidth, 8, FW_NORMAL);
    }

    int bodyHeight = 32 + (textHeight > 48 ? textHeight : 48) + 74;
    return ClampInt(kDialogHeaderHeight + bodyHeight, kDialogMinHeight, kDialogMaxHeight);
}

void LayoutDialogRects(VanguardDialogState* state) {
    if (!state) {
        return;
    }

    state->closeRect = MakeRect(state->width - 36, 9, state->width - 12, 33);

    int buttonTop = state->height - 54;
    int buttonWidth = 118;
    bool hasSecondary = HasText(state->config.secondaryButton);
    if (hasSecondary) {
        state->primaryButtonRect = MakeRect(state->width - 24 - buttonWidth, buttonTop, state->width - 24, buttonTop + kDialogButtonHeight);
        state->secondaryButtonRect = MakeRect(state->primaryButtonRect.left - 12 - buttonWidth, buttonTop, state->primaryButtonRect.left - 12, buttonTop + kDialogButtonHeight);
    } else {
        state->primaryButtonRect = MakeRect(state->width - 24 - buttonWidth, buttonTop, state->width - 24, buttonTop + kDialogButtonHeight);
        state->secondaryButtonRect = MakeRect(0, 0, 0, 0);
    }
}

bool EnsureVanguardDialogClass() {
    HINSTANCE instance = g_instance ? g_instance : GetModuleHandleW(nullptr);

    if (!g_iconSmall && instance) {
        g_iconSmall = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_NEXORA), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    }
    if (!g_iconLarge && instance) {
        g_iconLarge = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_NEXORA), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    }

    WNDCLASSEXW existing = {};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(instance, kDialogClassName, &existing)) {
        return true;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = VanguardDialogProc;
    wc.hInstance = instance;
    wc.hIcon = g_iconLarge;
    wc.hIconSm = g_iconSmall;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kDialogClassName;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int DrawWrappedDialogText(HDC hdc, const RECT& bounds, const wchar_t* text, HFONT font, COLORREF color, UINT format) {
    if (!HasText(text)) {
        return bounds.top;
    }

    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);

    RECT measure = MakeRect(bounds.left, 0, bounds.right, 0);
    DrawTextW(hdc, text, -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_LEFT);

    RECT draw = MakeRect(bounds.left, bounds.top, bounds.right, bounds.top + (measure.bottom - measure.top));
    DrawTextW(hdc, text, -1, &draw, format);

    SelectObject(hdc, oldFont);
    return draw.bottom;
}

void DrawDialogIcon(HDC hdc, const RECT& rect, DialogKind kind, const Palette& p) {
    COLORREF accent = DialogAccent(kind, p);
    HPEN pen = CreatePen(PS_SOLID, 2, accent);
    HBRUSH brush = CreateSolidBrush(p.tooltip);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);

    int cx = (rect.left + rect.right) / 2;
    int cy = (rect.top + rect.bottom) / 2;

    if (kind == DialogWarning) {
        POINT points[3] = {
            { cx, rect.top + 3 },
            { rect.right - 4, rect.bottom - 4 },
            { rect.left + 4, rect.bottom - 4 }
        };
        Polygon(hdc, points, 3);
        MoveToEx(hdc, cx, cy - 6, nullptr);
        LineTo(hdc, cx, cy + 4);
        SetPixel(hdc, cx, cy + 10, accent);
    } else if (kind == DialogSecurity) {
        POINT shield[6] = {
            { cx, rect.top + 3 },
            { rect.right - 5, rect.top + 9 },
            { rect.right - 7, cy + 7 },
            { cx, rect.bottom - 3 },
            { rect.left + 7, cy + 7 },
            { rect.left + 5, rect.top + 9 }
        };
        Polygon(hdc, shield, 6);
        MoveToEx(hdc, cx, cy - 7, nullptr);
        LineTo(hdc, cx, cy + 4);
        SetPixel(hdc, cx, cy + 10, accent);
    } else {
        Ellipse(hdc, rect.left + 3, rect.top + 3, rect.right - 3, rect.bottom - 3);
        if (kind == DialogSuccess) {
            MoveToEx(hdc, cx - 9, cy, nullptr);
            LineTo(hdc, cx - 3, cy + 7);
            LineTo(hdc, cx + 10, cy - 8);
        } else if (kind == DialogError) {
            MoveToEx(hdc, cx - 7, cy - 7, nullptr);
            LineTo(hdc, cx + 8, cy + 8);
            MoveToEx(hdc, cx + 7, cy - 7, nullptr);
            LineTo(hdc, cx - 8, cy + 8);
        } else {
            MoveToEx(hdc, cx, cy - 7, nullptr);
            LineTo(hdc, cx, cy - 5);
            MoveToEx(hdc, cx, cy - 1, nullptr);
            LineTo(hdc, cx, cy + 10);
        }
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawDialogButton(HDC hdc, const RECT& rect, const wchar_t* text, bool hover, bool primary, DialogKind kind, const Palette& p, HFONT font) {
    COLORREF fill = hover ? p.rowHover : p.button;
    COLORREF border = primary ? DialogAccent(kind, p) : p.buttonBorder;
    COLORREF textColor = primary ? DialogAccent(kind, p) : p.text;

    FillRound(hdc, rect, 7, fill, border);
    DrawTextLine(hdc, rect, text, font, textColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void DrawVanguardDialog(HWND hwnd, HDC hdc, VanguardDialogState* state) {
    if (!state) {
        return;
    }

    RECT client = {};
    GetClientRect(hwnd, &client);

    HDC memory = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, client.right - client.left, client.bottom - client.top);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

    Palette p = GetPalette();
    FillRound(memory, client, 12, p.tooltip, p.border);

    RECT header = MakeRect(0, 0, state->width, kDialogHeaderHeight);
    FillSolid(memory, header, p.header);

    if (g_iconSmall) {
        DrawIconEx(memory, 15, 13, g_iconSmall, 16, 16, 0, nullptr, DI_NORMAL);
    }

    HFONT titleFont = CreateUiFont(memory, 8, FW_BOLD);
    HFONT closeFont = CreateUiFont(memory, 10, FW_BOLD);
    HFONT primaryFont = CreateUiFont(memory, 13, FW_BOLD);
    HFONT textFont = CreateUiFont(memory, 9, FW_NORMAL);
    HFONT detailFont = CreateUiFont(memory, 8, FW_NORMAL);
    HFONT buttonFont = CreateUiFont(memory, 9, FW_SEMIBOLD);

    RECT title = MakeRect(38, 9, state->width - 48, 33);
    DrawTextLine(memory, title, HasText(state->config.title) ? state->config.title : kWindowTitle, titleFont, p.text, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    COLORREF closeColor = state->closeHover ? p.red : p.muted;
    DrawTextLine(memory, state->closeRect, L"x", closeFont, closeColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    RECT iconRect = MakeRect(28, 70, 70, 112);
    DrawDialogIcon(memory, iconRect, state->config.kind, p);

    RECT textBounds = MakeRect(88, 66, state->width - 28, state->height - 74);
    int nextTop = DrawWrappedDialogText(memory, textBounds, state->config.primaryText, primaryFont, p.text, DT_WORDBREAK | DT_LEFT);
    if (HasText(state->config.secondaryText)) {
        RECT secondary = MakeRect(textBounds.left, nextTop + 9, textBounds.right, textBounds.bottom);
        nextTop = DrawWrappedDialogText(memory, secondary, state->config.secondaryText, textFont, p.muted, DT_WORDBREAK | DT_LEFT);
    }
    if (HasText(state->config.detailText)) {
        RECT detail = MakeRect(textBounds.left, nextTop + 10, textBounds.right, textBounds.bottom);
        DrawWrappedDialogText(memory, detail, state->config.detailText, detailFont, p.text, DT_WORDBREAK | DT_LEFT);
    }

    DrawDialogButton(memory, state->primaryButtonRect, HasText(state->config.primaryButton) ? state->config.primaryButton : L"OK", state->primaryHover, true, state->config.kind, p, buttonFont);
    if (HasText(state->config.secondaryButton)) {
        DrawDialogButton(memory, state->secondaryButtonRect, state->config.secondaryButton, state->secondaryHover, false, state->config.kind, p, buttonFont);
    }

    BitBlt(hdc, 0, 0, client.right - client.left, client.bottom - client.top, memory, 0, 0, SRCCOPY);

    DeleteObject(buttonFont);
    DeleteObject(detailFont);
    DeleteObject(textFont);
    DeleteObject(primaryFont);
    DeleteObject(closeFont);
    DeleteObject(titleFont);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

void CompleteVanguardDialog(HWND hwnd, VanguardDialogState* state, int result) {
    if (!state || state->done) {
        return;
    }

    state->result = result;
    state->done = true;
    DestroyWindow(hwnd);
}

void UpdateDialogHover(HWND hwnd, VanguardDialogState* state, int x, int y) {
    if (!state) {
        return;
    }

    bool closeHover = ContainsPoint(state->closeRect, x, y);
    bool primaryHover = ContainsPoint(state->primaryButtonRect, x, y);
    bool secondaryHover = HasText(state->config.secondaryButton) && ContainsPoint(state->secondaryButtonRect, x, y);
    if (closeHover != state->closeHover || primaryHover != state->primaryHover || secondaryHover != state->secondaryHover) {
        state->closeHover = closeHover;
        state->primaryHover = primaryHover;
        state->secondaryHover = secondaryHover;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK VanguardDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    VanguardDialogState* state = reinterpret_cast<VanguardDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<VanguardDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC paint = BeginPaint(hwnd, &ps);
        DrawVanguardDialog(hwnd, paint, state);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
        UpdateDialogHover(hwnd, state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_NCHITTEST: {
        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &point);
        if (state && point.y >= 0 && point.y < kDialogHeaderHeight && !ContainsPoint(state->closeRect, point.x, point.y)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_LBUTTONUP:
        if (state) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (ContainsPoint(state->closeRect, x, y)) {
                CompleteVanguardDialog(hwnd, state, HasText(state->config.secondaryButton) ? IDNO : IDOK);
                return 0;
            }
            if (ContainsPoint(state->primaryButtonRect, x, y)) {
                CompleteVanguardDialog(hwnd, state, HasText(state->config.secondaryButton) ? IDYES : IDOK);
                return 0;
            }
            if (HasText(state->config.secondaryButton) && ContainsPoint(state->secondaryButtonRect, x, y)) {
                CompleteVanguardDialog(hwnd, state, IDNO);
                return 0;
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (state && wParam == VK_RETURN) {
            CompleteVanguardDialog(hwnd, state, HasText(state->config.secondaryButton) ? IDYES : IDOK);
            return 0;
        }
        if (state && wParam == VK_ESCAPE) {
            CompleteVanguardDialog(hwnd, state, HasText(state->config.secondaryButton) ? IDNO : IDOK);
            return 0;
        }
        break;

    case WM_CLOSE:
        CompleteVanguardDialog(hwnd, state, HasText(state->config.secondaryButton) ? IDNO : IDOK);
        return 0;

    case WM_DESTROY:
        if (state) {
            state->done = true;
        }
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int VanguardMessageBox(HWND owner, const VanguardDialogConfig& config) {
    if (!EnsureVanguardDialogClass()) {
        return IDOK;
    }

    VanguardDialogState state = {};
    state.config = config;
    state.result = HasText(config.secondaryButton) ? IDNO : IDOK;
    state.width = kDialogWidth;
    state.height = CalculateDialogHeight(config);
    LayoutDialogRects(&state);

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + ((work.right - work.left) - state.width) / 2;
    int y = work.top + ((work.bottom - work.top) - state.height) / 2;

    if (owner && IsWindow(owner)) {
        RECT ownerRect = {};
        GetWindowRect(owner, &ownerRect);
        x = ownerRect.left + ((ownerRect.right - ownerRect.left) - state.width) / 2;
        y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - state.height) / 2;
        x = ClampInt(x, work.left + 8, work.right - state.width - 8);
        y = ClampInt(y, work.top + 8, work.bottom - state.height - 8);
    }

    DWORD exStyle = WS_EX_TOOLWINDOW;
    if (config.topMost) {
        exStyle |= WS_EX_TOPMOST;
    }

    HWND dialog = CreateWindowExW(
        exStyle,
        kDialogClassName,
        HasText(config.title) ? config.title : kWindowTitle,
        WS_POPUP,
        x,
        y,
        state.width,
        state.height,
        owner,
        nullptr,
        g_instance ? g_instance : GetModuleHandleW(nullptr),
        &state);

    if (!dialog) {
        return state.result;
    }

    HRGN region = CreateRoundRectRgn(0, 0, state.width + 1, state.height + 1, 12, 12);
    SetWindowRgn(dialog, region, TRUE);

    bool ownerEnabled = false;
    if (owner && IsWindow(owner) && IsWindowEnabled(owner)) {
        ownerEnabled = true;
        EnableWindow(owner, FALSE);
    }

    ShowWindow(dialog, SW_SHOWNORMAL);
    SetForegroundWindow(dialog);

    MSG message = {};
    while (!state.done) {
        BOOL getResult = GetMessageW(&message, nullptr, 0, 0);
        if (getResult <= 0) {
            if (getResult == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            break;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (ownerEnabled && owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    return state.result;
}

int ShowThemedDialog(
    HWND owner,
    DialogKind kind,
    const wchar_t* title,
    const wchar_t* primaryText,
    const wchar_t* secondaryText,
    const wchar_t* detailText,
    const wchar_t* primaryButton,
    bool topMost) {
    VanguardDialogConfig config = {};
    config.kind = kind;
    config.title = title;
    config.primaryText = primaryText;
    config.secondaryText = secondaryText;
    config.detailText = detailText;
    config.primaryButton = primaryButton;
    config.secondaryButton = nullptr;
    config.topMost = topMost;
    return VanguardMessageBox(owner, config);
}

int ShowThemedConfirm(
    HWND owner,
    DialogKind kind,
    const wchar_t* title,
    const wchar_t* primaryText,
    const wchar_t* secondaryText,
    const wchar_t* detailText,
    const wchar_t* primaryButton,
    const wchar_t* secondaryButton) {
    VanguardDialogConfig config = {};
    config.kind = kind;
    config.title = title;
    config.primaryText = primaryText;
    config.secondaryText = secondaryText;
    config.detailText = detailText;
    config.primaryButton = primaryButton;
    config.secondaryButton = secondaryButton;
    config.topMost = false;
    return VanguardMessageBox(owner, config);
}

void ShowDualBoxBlockedDialogModal(HWND owner) {
    ShowThemedDialog(
        owner,
        DialogSecurity,
        kWindowTitle,
        L"Dual box bloqueado",
        L"Por seguranca e equilibrio do servidor, e permitido apenas 1 cliente do Aion por computador.",
        L"Feche o cliente ja aberto antes de iniciar uma nova sessao.",
        L"Entendi",
        true);
}

RECT ThemeButtonRect() {
    return MakeRect(kWindowWidth - 92, 8, kWindowWidth - 64, 31);
}

RECT MenuButtonRect() {
    return MakeRect(kWindowWidth - 62, 8, kWindowWidth - 34, 31);
}

RECT CloseButtonRect() {
    return MakeRect(kWindowWidth - 32, 8, kWindowWidth - 8, 31);
}

RECT FooterButtonRect() {
    return MakeRect(26, 320, kWindowWidth - 26, 354);
}

void DrawThemeButton(HDC hdc, const RECT& rect, const Palette& p) {
    COLORREF color = g_theme == ThemeDark ? RGB(220, 226, 236) : RGB(88, 99, 115);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);

    int cx = (rect.left + rect.right) / 2;
    int cy = (rect.top + rect.bottom) / 2;

    if (g_theme == ThemeDark) {
        Ellipse(hdc, cx - 4, cy - 4, cx + 5, cy + 5);
        MoveToEx(hdc, cx, cy - 8, nullptr);
        LineTo(hdc, cx, cy - 6);
        MoveToEx(hdc, cx, cy + 7, nullptr);
        LineTo(hdc, cx, cy + 9);
        MoveToEx(hdc, cx - 8, cy, nullptr);
        LineTo(hdc, cx - 6, cy);
        MoveToEx(hdc, cx + 7, cy, nullptr);
        LineTo(hdc, cx + 9, cy);
        MoveToEx(hdc, cx - 6, cy - 6, nullptr);
        LineTo(hdc, cx - 5, cy - 5);
        MoveToEx(hdc, cx + 6, cy - 6, nullptr);
        LineTo(hdc, cx + 5, cy - 5);
        MoveToEx(hdc, cx - 6, cy + 6, nullptr);
        LineTo(hdc, cx - 5, cy + 5);
        MoveToEx(hdc, cx + 6, cy + 6, nullptr);
        LineTo(hdc, cx + 5, cy + 5);
    } else {
        Ellipse(hdc, cx - 6, cy - 6, cx + 6, cy + 6);
        HBRUSH cutBrush = CreateSolidBrush(p.header);
        HPEN cutPen = CreatePen(PS_SOLID, 1, p.header);
        SelectObject(hdc, cutBrush);
        SelectObject(hdc, cutPen);
        Ellipse(hdc, cx - 1, cy - 8, cx + 9, cy + 4);
        SelectObject(hdc, brush);
        SelectObject(hdc, pen);
        DeleteObject(cutPen);
        DeleteObject(cutBrush);
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawHeader(HDC hdc, HFONT titleFont, HFONT iconFont, const Palette& p) {
    RECT header = MakeRect(0, 0, kWindowWidth, kHeaderHeight);
    FillSolid(hdc, header, p.header);

    if (g_iconSmall) {
        DrawIconEx(hdc, 13, 11, g_iconSmall, 16, 16, 0, nullptr, DI_NORMAL);
    }

    RECT title = MakeRect(35, 10, 190, 31);
    DrawTextLine(hdc, title, L"NEXORA VANGUARD", titleFont, p.text, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    DrawThemeButton(hdc, ThemeButtonRect(), p);

    RECT menu = MenuButtonRect();
    DrawTextLine(hdc, menu, L"...", iconFont, p.muted, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    RECT close = CloseButtonRect();
    DrawTextLine(hdc, close, L"x", iconFont, p.muted, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

COLORREF StatusColor(StatusKind status, const Palette& p) {
    if (status == StatusOk) {
        return p.green;
    }
    if (status == StatusWarning) {
        return p.yellow;
    }
    return p.red;
}

void DrawRequirementRows(HDC hdc, HFONT rowFont, const Palette& p) {
    for (int i = 0; i < kRowCount; ++i) {
        RECT row = g_requirements[i].rect;
        COLORREF rowColor = i == g_hoverRow ? p.rowHover : p.row;
        FillRound(hdc, row, 8, rowColor, p.border);

        RECT stripe = MakeRect(row.left, row.top + 7, row.left + 4, row.bottom - 7);
        FillSolid(hdc, stripe, StatusColor(g_requirements[i].status, p));

        RECT title = MakeRect(row.left + 14, row.top, row.right - 42, row.bottom);
        DrawTextLine(hdc, title, g_requirements[i].title, rowFont, p.text, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        RECT mark = MakeRect(row.right - 33, row.top + 4, row.right - 12, row.bottom - 4);
        DrawStatusMark(hdc, mark, g_requirements[i].status, p);
    }
}

bool HasUnmetRequirement() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_requirements[i].status != StatusOk) {
            return true;
        }
    }
    return false;
}

void DrawFooterButton(HDC hdc, HFONT font, const Palette& p) {
    RECT button = FooterButtonRect();
    COLORREF buttonColor = g_hoverFooter ? p.rowHover : p.button;
    FillRound(hdc, button, 5, buttonColor, p.buttonBorder);

    bool unmet = HasUnmetRequirement();
    COLORREF textColor = unmet ? p.disabledText : p.green;
    const wchar_t* text = unmet ? L"Requisito nao atendido" : L"Protecao ativa";

    SIZE textSize = {};
    HGDIOBJ oldFontForMeasure = SelectObject(hdc, font);
    GetTextExtentPoint32W(hdc, text, lstrlenW(text), &textSize);
    SelectObject(hdc, oldFontForMeasure);

    const int iconWidth = 19;
    const int gap = 8;
    const int groupWidth = iconWidth + gap + textSize.cx;
    int groupLeft = button.left + ((button.right - button.left) - groupWidth) / 2;
    if (groupLeft < button.left + 12) {
        groupLeft = button.left + 12;
    }

    RECT icon = MakeRect(groupLeft, button.top + 8, groupLeft + iconWidth, button.bottom - 8);
    HPEN pen = CreatePen(PS_SOLID, 2, textColor);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    if (unmet) {
        int cx = (icon.left + icon.right) / 2;
        MoveToEx(hdc, icon.left + 3, icon.top + 2, nullptr);
        LineTo(hdc, cx, icon.bottom - 2);
        LineTo(hdc, icon.right - 3, icon.top + 2);
        MoveToEx(hdc, icon.left + 6, icon.top + 2, nullptr);
        LineTo(hdc, cx, icon.bottom - 7);
        LineTo(hdc, icon.right - 6, icon.top + 2);
    } else {
        MoveToEx(hdc, icon.left + 3, icon.top + 8, nullptr);
        LineTo(hdc, icon.left + 8, icon.bottom - 4);
        LineTo(hdc, icon.right - 3, icon.top + 3);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    RECT label = MakeRect(groupLeft + iconWidth + gap, button.top, button.right - 12, button.bottom);
    DrawTextLine(hdc, label, text, font, textColor, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

void DrawRequirementTooltip(HDC hdc, HFONT font, const Palette& p) {
    if (g_hoverRow < 0 || g_hoverRow >= kRowCount) {
        return;
    }

    const Requirement& item = g_requirements[g_hoverRow];
    RECT measure = MakeRect(0, 0, 228, 0);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    DrawTextW(hdc, item.detail, -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
    SelectObject(hdc, oldFont);

    int height = (measure.bottom - measure.top) + 28;
    if (height < 78) {
        height = 78;
    }

    int left = 20;
    int top = item.rect.bottom + 6;
    if (top + height > 316) {
        top = item.rect.top - height - 8;
    }
    if (top < 68) {
        top = 68;
    }

    RECT box = MakeRect(left, top, left + 258, top + height);
    FillRound(hdc, box, 8, p.tooltip, p.border);

    POINT arrow[3];
    int cx = (item.rect.left + item.rect.right) / 2;
    if (box.top > item.rect.top) {
        arrow[0] = { cx - 7, box.top };
        arrow[1] = { cx + 7, box.top };
        arrow[2] = { cx, box.top - 8 };
    } else {
        arrow[0] = { cx - 7, box.bottom };
        arrow[1] = { cx + 7, box.bottom };
        arrow[2] = { cx, box.bottom + 8 };
    }

    HBRUSH brush = CreateSolidBrush(p.tooltip);
    HPEN pen = CreatePen(PS_SOLID, 1, p.border);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    Polygon(hdc, arrow, 3);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    RECT text = MakeRect(box.left + 14, box.top + 13, box.right - 14, box.bottom - 12);
    DrawTextLine(hdc, text, item.detail, font, p.text, DT_LEFT | DT_WORDBREAK);
}

void DrawFooterTooltip(HDC hdc, HFONT font, const Palette& p) {
    if (!g_hoverFooter || !HasUnmetRequirement()) {
        return;
    }

    const wchar_t* detail =
        L"Voce ainda pode jogar, mas este PC nao atende aos requisitos para que o Nexora Vanguard fique ativo apenas durante a partida.";

    RECT measure = MakeRect(0, 0, 242, 0);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    DrawTextW(hdc, detail, -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
    SelectObject(hdc, oldFont);

    int height = (measure.bottom - measure.top) + 28;
    if (height < 84) {
        height = 84;
    }

    RECT button = FooterButtonRect();
    int top = button.top - height - 14;
    if (top < kHeaderHeight + 8) {
        top = kHeaderHeight + 8;
    }

    RECT box = MakeRect(15, top, kWindowWidth - 15, top + height);
    FillRound(hdc, box, 8, p.tooltip, p.border);

    int cx = (button.left + button.right) / 2;
    POINT arrow[3] = {
        { cx - 7, box.bottom },
        { cx + 7, box.bottom },
        { cx, box.bottom + 8 }
    };

    HBRUSH brush = CreateSolidBrush(p.tooltip);
    HPEN pen = CreatePen(PS_SOLID, 1, p.border);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    Polygon(hdc, arrow, 3);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    RECT text = MakeRect(box.left + 14, box.top + 13, box.right - 14, box.bottom - 12);
    DrawTextLine(hdc, text, detail, font, p.text, DT_LEFT | DT_WORDBREAK);
}

void DrawTooltips(HDC hdc, HFONT font, const Palette& p) {
    DrawRequirementTooltip(hdc, font, p);
    DrawFooterTooltip(hdc, font, p);
}

void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps = {};
    HDC hdc = BeginPaint(hwnd, &ps);
    HDC memory = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, kWindowWidth, kWindowHeight);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

    Palette p = GetPalette();
    RECT frame = MakeRect(0, 0, kWindowWidth, kWindowHeight);
    FillSolid(memory, frame, p.frame);

    HFONT titleFont = CreateUiFont(memory, 8, FW_BOLD);
    HFONT smallFont = CreateUiFont(memory, 8, FW_NORMAL);
    HFONT rowFont = CreateUiFont(memory, 9, FW_BOLD);
    HFONT detailFont = CreateUiFont(memory, 9, FW_NORMAL);
    HFONT footerFont = CreateUiFont(memory, 9, FW_BOLD);
    HFONT iconFont = CreateUiFont(memory, 10, FW_BOLD);

    DrawHeader(memory, titleFont, iconFont, p);

    RECT subtitle = MakeRect(16, 51, kWindowWidth - 16, 68);
    DrawTextLine(memory, subtitle, L"RECURSOS DE SEGURANCA RECOMENDADOS", smallFont, p.muted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    DrawRequirementRows(memory, rowFont, p);
    DrawFooterButton(memory, footerFont, p);
    DrawTooltips(memory, detailFont, p);

    BitBlt(hdc, 0, 0, kWindowWidth, kWindowHeight, memory, 0, 0, SRCCOPY);

    DeleteObject(iconFont);
    DeleteObject(footerFont);
    DeleteObject(detailFont);
    DeleteObject(rowFont);
    DeleteObject(smallFont);
    DeleteObject(titleFont);

    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(hwnd, &ps);
}

bool ReadRegDword(HKEY root, const wchar_t* subkey, const wchar_t* valueName, DWORD* value) {
    if (!value) {
        return false;
    }

    HKEY key = nullptr;
    LONG openResult = RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (openResult != ERROR_SUCCESS) {
        openResult = RegOpenKeyExW(root, subkey, 0, KEY_READ, &key);
    }
    if (openResult != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = REG_DWORD;
    DWORD size = sizeof(DWORD);
    DWORD data = 0;
    LONG queryResult = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(&data), &size);
    RegCloseKey(key);

    if (queryResult != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(DWORD)) {
        return false;
    }

    *value = data;
    return true;
}

StatusKind DetectWindows25H2() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return StatusBad;
    }

    RtlGetVersionFn rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion) {
        return StatusBad;
    }

    RtlOsVersionInfoW os = {};
    os.dwOSVersionInfoSize = sizeof(os);
    if (rtlGetVersion(&os) != 0) {
        return StatusBad;
    }

    if (os.dwMajorVersion > 10 || (os.dwMajorVersion == 10 && os.dwBuildNumber >= 26200)) {
        return StatusOk;
    }

    return StatusBad;
}

StatusKind DetectSecureBoot() {
    DWORD enabled = 0;
    if (!ReadRegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", L"UEFISecureBootEnabled", &enabled)) {
        return StatusWarning;
    }

    return enabled ? StatusOk : StatusWarning;
}

StatusKind DetectTpm20() {
    HMODULE tbs = LoadLibraryW(L"tbs.dll");
    if (!tbs) {
        return StatusBad;
    }

    TbsiGetDeviceInfoFn getInfo = reinterpret_cast<TbsiGetDeviceInfoFn>(GetProcAddress(tbs, "Tbsi_GetDeviceInfo"));
    if (!getInfo) {
        FreeLibrary(tbs);
        return StatusBad;
    }

    TbsDeviceInfo info = {};
    info.structVersion = 1;
    UINT32 result = getInfo(sizeof(info), &info);
    FreeLibrary(tbs);

    if (result != 0) {
        return StatusBad;
    }

    return info.tpmVersion >= 2 ? StatusOk : StatusWarning;
}

StatusKind DetectVbs() {
    DWORD enabled = 0;
    if (!ReadRegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", L"EnableVirtualizationBasedSecurity", &enabled)) {
        return StatusBad;
    }

    return enabled ? StatusOk : StatusBad;
}

StatusKind DetectHvci() {
    DWORD enabled = 0;
    if (!ReadRegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", L"Enabled", &enabled)) {
        return StatusBad;
    }

    return enabled ? StatusOk : StatusBad;
}

StatusKind DetectIommu() {
    DWORD features = 0;
    if (ReadRegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", L"RequirePlatformSecurityFeatures", &features)) {
        return (features & 2) ? StatusOk : StatusWarning;
    }

    return StatusOk;
}

void SetRequirement(int index, const wchar_t* title, const wchar_t* detail, StatusKind status) {
    g_requirements[index].title = title;
    g_requirements[index].detail = detail;
    g_requirements[index].status = status;
    int top = 75 + index * 37;
    g_requirements[index].rect = MakeRect(16, top, kWindowWidth - 16, top + 31);
}

void RefreshRequirements() {
    SetRequirement(
        0,
        L"Windows 11 25H2 ou posterior",
        L"Manter o Windows atualizado ajuda a garantir recursos de seguranca recentes que o Nexora Vanguard usa durante o jogo.",
        DetectWindows25H2());

    SetRequirement(
        1,
        L"UEFI Secure Boot",
        L"O Secure Boot ajuda a garantir que apenas componentes confiaveis sejam carregados antes do Windows iniciar.",
        DetectSecureBoot());

    SetRequirement(
        2,
        L"TPM 2.0",
        L"O TPM 2.0 fornece uma raiz de confianca em hardware para verificar que o sistema foi iniciado com integridade.",
        DetectTpm20());

    SetRequirement(
        3,
        L"VBS",
        L"O VBS usa virtualizacao de hardware para isolar componentes criticos do sistema contra acesso nao autorizado.",
        DetectVbs());

    SetRequirement(
        4,
        L"HVCI",
        L"O HVCI aproveita o VBS para bloquear drivers de kernel nao assinados ou modificados.",
        DetectHvci());

    SetRequirement(
        5,
        L"IOMMU",
        L"O IOMMU ajuda a impedir que dispositivos com capacidade de DMA leiam ou gravem memoria protegida.",
        DetectIommu());
}

void AddTrayIcon(HWND hwnd) {
    ZeroMemory(&g_tray, sizeof(g_tray));
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = kTrayId;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_tray.uCallbackMessage = kTrayMessage;
    g_tray.hIcon = g_iconSmall ? g_iconSmall : LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_NEXORA));
    StringCchCopyW(g_tray.szTip, ARRAYSIZE(g_tray.szTip), L"Nexora Vanguard");

    if (Shell_NotifyIconW(NIM_ADD, &g_tray)) {
        g_tray.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_tray);
    }
}

void RemoveTrayIcon() {
    if (g_tray.cbSize != 0) {
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        ZeroMemory(&g_tray, sizeof(g_tray));
    }
}

void ReleaseInstanceMutex() {
    if (g_instanceMutex) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
    }
}

void PositionWindow(HWND hwnd);
void DestroyCustomMenu();

void ShowMainWindow(HWND hwnd) {
    if (InterlockedCompareExchange(&g_stopping, 0, 0) != 0) {
        return;
    }

    if (!IsWindowVisible(hwnd)) {
        PositionWindow(hwnd);
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }
    SetForegroundWindow(hwnd);
}

void HideMainWindow(HWND hwnd) {
    DestroyCustomMenu();
    ShowWindow(hwnd, SW_HIDE);
}

void ToggleMainWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    if (IsWindowVisible(hwnd)) {
        HideMainWindow(hwnd);
        return;
    }

    ShowMainWindow(hwnd);
}

void ToggleTheme(HWND hwnd) {
    g_theme = (g_theme == ThemeLight) ? ThemeDark : ThemeLight;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void SaveLogs(HWND hwnd) {
    wchar_t path[MAX_PATH] = {};
    if (!GetTempPathW(ARRAYSIZE(path), path)) {
        ShowThemedDialog(
            hwnd,
            DialogError,
            kWindowTitle,
            L"Nao foi possivel localizar a pasta temporaria.",
            L"O Windows nao retornou uma pasta temporaria valida para salvar o arquivo de log.",
            nullptr,
            L"Entendi",
            false);
        return;
    }

    StringCchCatW(path, ARRAYSIZE(path), L"NexoraVanguard.log");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ShowThemedDialog(
            hwnd,
            DialogError,
            kWindowTitle,
            L"Nao foi possivel salvar os logs.",
            L"Verifique as permissoes da pasta temporaria e tente novamente.",
            nullptr,
            L"Entendi",
            false);
        return;
    }

    const char* statusNames[] = { "OK", "AVISO", "FALHA" };
    char buffer[2048] = {};
    StringCchPrintfA(
        buffer,
        ARRAYSIZE(buffer),
        "Nexora Vanguard\r\n"
        "Windows: %s\r\n"
        "Secure Boot: %s\r\n"
        "TPM 2.0: %s\r\n"
        "VBS: %s\r\n"
        "HVCI: %s\r\n"
        "IOMMU: %s\r\n",
        statusNames[g_requirements[0].status],
        statusNames[g_requirements[1].status],
        statusNames[g_requirements[2].status],
        statusNames[g_requirements[3].status],
        statusNames[g_requirements[4].status],
        statusNames[g_requirements[5].status]);

    DWORD written = 0;
    WriteFile(file, buffer, static_cast<DWORD>(lstrlenA(buffer)), &written, nullptr);
    CloseHandle(file);

    wchar_t message[MAX_PATH + 64] = {};
    StringCchPrintfW(message, ARRAYSIZE(message), L"Logs salvos em:\n%s", path);
    ShowThemedDialog(
        hwnd,
        DialogSuccess,
        kWindowTitle,
        L"Logs salvos",
        message,
        nullptr,
        L"Entendi",
        false);
}

void ConfirmExit(HWND hwnd) {
    int result = ShowThemedConfirm(
        hwnd,
        DialogWarning,
        L"Sair do Vanguard",
        L"Encerrar o Nexora Vanguard?",
        L"Sair do Nexora Vanguard com o Aion aberto ira fechar o jogo.",
        L"Use esta opcao apenas quando quiser finalizar a protecao e fechar o cliente.",
        L"Sair",
        L"Cancelar");

    if (result == IDYES) {
        RemoveTrayIcon();
        ExitProcess(0);
    }
}

void RunPrecheck(HWND hwnd) {
    RefreshRequirements();
    InvalidateRect(hwnd, nullptr, FALSE);
    ShowThemedDialog(
        hwnd,
        DialogSuccess,
        kWindowTitle,
        L"Verificacao previa concluida",
        L"Os requisitos de seguranca foram verificados novamente.",
        nullptr,
        L"Entendi",
        false);
}

void AddMenuItem(const wchar_t* text, UINT command, bool enabled, bool checked) {
    if (g_menuHeight <= 0) {
        g_menuHeight = kMenuPadding;
    }
    if (g_menuItemCount >= kMaxMenuItems) {
        return;
    }

    MenuItem& item = g_menuItems[g_menuItemCount++];
    item.text = text;
    item.command = command;
    item.enabled = enabled;
    item.separator = false;
    item.checked = checked;
    item.rect = MakeRect(kMenuPadding, g_menuHeight, kMenuWidth - kMenuPadding, g_menuHeight + kMenuItemHeight);
    g_menuHeight += kMenuItemHeight;
}

void AddMenuSeparator() {
    if (g_menuHeight <= 0) {
        g_menuHeight = kMenuPadding;
    }
    if (g_menuItemCount >= kMaxMenuItems) {
        return;
    }

    MenuItem& item = g_menuItems[g_menuItemCount++];
    item.text = nullptr;
    item.command = 0;
    item.enabled = false;
    item.separator = true;
    item.checked = false;
    item.rect = MakeRect(kMenuPadding + 8, g_menuHeight + 4, kMenuWidth - kMenuPadding - 8, g_menuHeight + 5);
    g_menuHeight += kMenuSeparatorHeight;
}

void BuildCustomMenu(HWND owner) {
    ZeroMemory(g_menuItems, sizeof(g_menuItems));
    g_menuOwner = owner;
    g_menuHover = -1;
    g_menuItemCount = 0;
    g_menuHeight = kMenuPadding;

    if (!IsWindowVisible(owner)) {
        AddMenuItem(L"Abrir painel", IDM_OPEN, true, false);
        AddMenuSeparator();
    }

    AddMenuItem(L"Salvar logs...", IDM_SAVE_LOGS, true, false);
    AddMenuItem(L"Verificacao previa do Vanguard...", IDM_PRECHECK, true, false);
    AddMenuSeparator();
    AddMenuItem(L"Portugues (Brasil)", IDM_LANG_PTBR, false, true);
    AddMenuItem(L"Iniciar com o Windows", 0, false, false);
    AddMenuItem(L"Modo silencioso", 0, false, false);
    AddMenuSeparator();
    AddMenuItem(g_theme == ThemeLight ? L"Tema escuro" : L"Tema claro", IDM_TOGGLE_THEME, true, false);
    AddMenuItem(L"Sair do Vanguard", IDM_EXIT, true, false);

    g_menuHeight += kMenuPadding;
}

int HitMenuItem(int x, int y) {
    for (int i = 0; i < g_menuItemCount; ++i) {
        if (!g_menuItems[i].separator && ContainsPoint(g_menuItems[i].rect, x, y)) {
            return i;
        }
    }

    return -1;
}

void DrawMenuCheck(HDC hdc, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    int cx = (rect.left + rect.right) / 2;
    int cy = (rect.top + rect.bottom) / 2;
    MoveToEx(hdc, cx - 6, cy, nullptr);
    LineTo(hdc, cx - 2, cy + 5);
    LineTo(hdc, cx + 7, cy - 6);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void DrawMenuWindow(HWND hwnd, HDC hdc) {
    RECT client = {};
    GetClientRect(hwnd, &client);

    HDC memory = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, client.right - client.left, client.bottom - client.top);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

    Palette p = GetPalette();
    FillRound(memory, client, 10, p.tooltip, p.border);

    HFONT itemFont = CreateUiFont(memory, 9, FW_SEMIBOLD);
    HFONT smallFont = CreateUiFont(memory, 8, FW_NORMAL);

    for (int i = 0; i < g_menuItemCount; ++i) {
        const MenuItem& item = g_menuItems[i];
        if (item.separator) {
            FillSolid(memory, item.rect, p.border);
            continue;
        }

        if (i == g_menuHover && item.enabled) {
            FillRound(memory, item.rect, 7, p.rowHover, p.rowHover);
        }

        COLORREF textColor = item.enabled ? p.text : p.disabledText;
        if (item.command == IDM_EXIT && item.enabled) {
            textColor = p.red;
        }

        RECT marker = MakeRect(item.rect.left + 10, item.rect.top + 7, item.rect.left + 26, item.rect.bottom - 7);
        if (item.checked) {
            DrawMenuCheck(memory, marker, p.green);
        }

        RECT text = MakeRect(item.rect.left + 34, item.rect.top, item.rect.right - 12, item.rect.bottom);
        DrawTextLine(memory, text, item.text, item.enabled ? itemFont : smallFont, textColor, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    BitBlt(hdc, 0, 0, client.right - client.left, client.bottom - client.top, memory, 0, 0, SRCCOPY);

    DeleteObject(smallFont);
    DeleteObject(itemFont);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

void DestroyCustomMenu() {
    if (g_menuWindow && IsWindow(g_menuWindow)) {
        DestroyWindow(g_menuWindow);
        return;
    }

    g_menuWindow = nullptr;
    g_menuOwner = nullptr;
    g_menuHover = -1;
    g_menuItemCount = 0;
    g_menuHeight = 0;
}

void AdjustMenuPosition(POINT* point, int width, int height) {
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    if (point->x + width > work.right) {
        point->x = work.right - width - 4;
    }
    if (point->y + height > work.bottom) {
        point->y = work.bottom - height - 4;
    }
    if (point->x < work.left) {
        point->x = work.left + 4;
    }
    if (point->y < work.top) {
        point->y = work.top + 4;
    }
}

LRESULT CALLBACK MenuWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawMenuWindow(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int hover = HitMenuItem(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (hover >= 0 && !g_menuItems[hover].enabled) {
            hover = -1;
        }
        if (hover != g_menuHover) {
            g_menuHover = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int hit = HitMenuItem(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        HWND owner = g_menuOwner;
        UINT command = 0;

        if (hit >= 0 && g_menuItems[hit].enabled) {
            command = g_menuItems[hit].command;
        }

        DestroyCustomMenu();

        if (owner && command != 0) {
            SendMessageW(owner, WM_COMMAND, MAKEWPARAM(command, 0), 0);
        }
        return 0;
    }

    case WM_RBUTTONUP:
    case WM_CANCELMODE:
    case WM_KILLFOCUS:
        DestroyCustomMenu();
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyCustomMenu();
            return 0;
        }
        break;

    case WM_DESTROY:
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        if (g_menuWindow == hwnd) {
            g_menuWindow = nullptr;
            g_menuOwner = nullptr;
            g_menuHover = -1;
            g_menuItemCount = 0;
            g_menuHeight = 0;
        }
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ShowMenu(HWND hwnd, POINT screenPoint) {
    DestroyCustomMenu();
    BuildCustomMenu(hwnd);
    AdjustMenuPosition(&screenPoint, kMenuWidth, g_menuHeight);

    g_menuWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kMenuClassName,
        L"",
        WS_POPUP,
        screenPoint.x,
        screenPoint.y,
        kMenuWidth,
        g_menuHeight,
        hwnd,
        nullptr,
        g_instance,
        nullptr);

    if (!g_menuWindow) {
        return;
    }

    HRGN region = CreateRoundRectRgn(0, 0, kMenuWidth + 1, g_menuHeight + 1, 10, 10);
    SetWindowRgn(g_menuWindow, region, TRUE);
    ShowWindow(g_menuWindow, SW_SHOWNOACTIVATE);
    SetCapture(g_menuWindow);
}

int HitRequirement(int x, int y) {
    for (int i = 0; i < kRowCount; ++i) {
        if (ContainsPoint(g_requirements[i].rect, x, y)) {
            return i;
        }
    }

    return -1;
}

void UpdateHover(HWND hwnd, int x, int y) {
    int nextHover = HitRequirement(x, y);
    bool nextFooterHover = ContainsPoint(FooterButtonRect(), x, y);
    if (nextHover != g_hoverRow || nextFooterHover != g_hoverFooter) {
        g_hoverRow = nextHover;
        g_hoverFooter = nextFooterHover;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    if (!g_trackingMouse) {
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        g_trackingMouse = true;
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        AddTrayIcon(hwnd);
        return 0;

    case kShutdownMessage:
        DestroyCustomMenu();
        RemoveTrayIcon();
        ShowWindow(hwnd, SW_HIDE);
        DestroyWindow(hwnd);
        return 0;

    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        UpdateHover(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSELEAVE:
        g_trackingMouse = false;
        if (g_hoverRow != -1 || g_hoverFooter) {
            g_hoverRow = -1;
            g_hoverFooter = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_NCHITTEST: {
        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &point);
        if (point.y >= 0 && point.y < kHeaderHeight &&
            !ContainsPoint(ThemeButtonRect(), point.x, point.y) &&
            !ContainsPoint(MenuButtonRect(), point.x, point.y) &&
            !ContainsPoint(CloseButtonRect(), point.x, point.y)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        if (ContainsPoint(ThemeButtonRect(), x, y)) {
            ToggleTheme(hwnd);
            return 0;
        }

        if (ContainsPoint(MenuButtonRect(), x, y)) {
            RECT menuRect = MenuButtonRect();
            POINT point = { menuRect.left, menuRect.bottom + 5 };
            ClientToScreen(hwnd, &point);
            ShowMenu(hwnd, point);
            return 0;
        }

        if (ContainsPoint(CloseButtonRect(), x, y)) {
            HideMainWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hwnd, &point);
        ShowMenu(hwnd, point);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_OPEN:
            ToggleMainWindow(hwnd);
            return 0;
        case IDM_SAVE_LOGS:
            SaveLogs(hwnd);
            return 0;
        case IDM_PRECHECK:
            RunPrecheck(hwnd);
            return 0;
        case IDM_TOGGLE_THEME:
            ToggleTheme(hwnd);
            return 0;
        case IDM_EXIT:
            ConfirmExit(hwnd);
            return 0;
        default:
            return 0;
        }

    case kTrayMessage:
        if (InterlockedCompareExchange(&g_stopping, 0, 0) != 0) {
            return 0;
        }

        if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT) {
            ToggleMainWindow(hwnd);
            return 0;
        }
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            POINT point = {};
            GetCursorPos(&point);
            ShowMenu(hwnd, point);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        HideMainWindow(hwnd);
        return 0;

    case WM_QUERYENDSESSION:
        DestroyCustomMenu();
        RemoveTrayIcon();
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_ENDSESSION:
        if (wParam) {
            DestroyCustomMenu();
            RemoveTrayIcon();
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;

    case WM_DESTROY:
        DestroyCustomMenu();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void PositionWindow(HWND hwnd) {
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.right - kWindowWidth - 14;
    int y = work.bottom - kWindowHeight - 14;
    SetWindowPos(hwnd, HWND_TOP, x, y, kWindowWidth, kWindowHeight, SWP_NOACTIVATE);
}

DWORD WINAPI UiThread(LPVOID parameter) {
    g_instance = reinterpret_cast<HINSTANCE>(parameter);

    g_iconSmall = reinterpret_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_NEXORA), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    g_iconLarge = reinterpret_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_NEXORA), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    RefreshRequirements();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = g_instance;
    wc.hIcon = g_iconLarge;
    wc.hIconSm = g_iconSmall;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);

    WNDCLASSEXW menuWc = {};
    menuWc.cbSize = sizeof(menuWc);
    menuWc.lpfnWndProc = MenuWindowProc;
    menuWc.hInstance = g_instance;
    menuWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    menuWc.lpszClassName = kMenuClassName;
    RegisterClassExW(&menuWc);

    g_window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kWindowClassName,
        kWindowTitle,
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kWindowWidth,
        kWindowHeight,
        nullptr,
        nullptr,
        g_instance,
        nullptr);

    if (!g_window) {
        return 0;
    }

    HRGN region = CreateRoundRectRgn(0, 0, kWindowWidth + 1, kWindowHeight + 1, 10, 10);
    SetWindowRgn(g_window, region, TRUE);
    PositionWindow(g_window);
    ShowWindow(g_window, SW_HIDE);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    RemoveTrayIcon();

    if (g_iconLarge) {
        DestroyIcon(g_iconLarge);
        g_iconLarge = nullptr;
    }
    if (g_iconSmall) {
        DestroyIcon(g_iconSmall);
        g_iconSmall = nullptr;
    }

    g_window = nullptr;
    g_uiThreadId = 0;
    InterlockedExchange(&g_stopping, 0);
    InterlockedExchange(&g_started, 0);
    ReleaseInstanceMutex();

    return 0;
}

void StartNexoraVanguard(HINSTANCE instance) {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        return;
    }

    InterlockedExchange(&g_stopping, 0);

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) {
            CloseHandle(mutex);
        }
        InterlockedExchange(&g_started, 0);
        return;
    }

    g_instanceMutex = mutex;

    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, UiThread, instance, 0, &threadId);
    if (thread) {
        g_uiThreadId = threadId;
        CloseHandle(thread);
    } else {
        ReleaseInstanceMutex();
        InterlockedExchange(&g_started, 0);
    }
}

void StopNexoraVanguard() {
    if (InterlockedExchange(&g_stopping, 1) == 0) {
        RemoveTrayIcon();

        HWND hwnd = g_window;
        if (hwnd && IsWindow(hwnd)) {
            ShowWindow(hwnd, SW_HIDE);
            PostMessageW(hwnd, kShutdownMessage, 0, 0);
            return;
        }

        if (g_uiThreadId != 0) {
            PostThreadMessageW(g_uiThreadId, WM_QUIT, 0, 0);
        }
    }
}

} // namespace

extern "C" __declspec(dllexport) void WINAPI StartNexoraVanguardUi() {
    StartNexoraVanguard(g_instance ? g_instance : GetModuleHandleW(L"NexoraVanguard.dll"));
}

extern "C" __declspec(dllexport) void WINAPI StopNexoraVanguardUi() {
    StopNexoraVanguard();
}

extern "C" __declspec(dllexport) void CALLBACK ShowDualBoxBlockedDialog(HWND, HINSTANCE instance, LPSTR, int) {
    if (instance) {
        g_instance = instance;
    }
    ShowDualBoxBlockedDialogModal(nullptr);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        g_instance = instance;
        if (IsRundll32HostProcess()) {
            return TRUE;
        }
        if (!CanStartGameClient()) {
            BlockExtraGameClient();
            return TRUE;
        }
        StartNexoraVanguard(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        StopNexoraVanguard();
        ReleaseGameClientSlot();
    }

    return TRUE;
}
