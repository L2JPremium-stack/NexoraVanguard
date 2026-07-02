#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdint.h>

#include "resource.h"

namespace {

const wchar_t kWindowClassName[] = L"NexoraVanguardWindow";
const wchar_t kWindowTitle[] = L"Nexora Vanguard";
const UINT kTrayMessage = WM_APP + 45;
const UINT_PTR kTrayId = 1;

const int kWindowWidth = 300;
const int kWindowHeight = 382;
const int kHeaderHeight = 38;
const int kRowCount = 6;

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
NOTIFYICONDATAW g_tray = {};
HICON g_iconSmall = nullptr;
HICON g_iconLarge = nullptr;
ThemeKind g_theme = ThemeLight;
Requirement g_requirements[kRowCount] = {};
int g_hoverRow = -1;
bool g_trackingMouse = false;
volatile LONG g_started = 0;

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect = { left, top, right, bottom };
    return rect;
}

bool ContainsPoint(const RECT& rect, int x, int y) {
    POINT point = { x, y };
    return PtInRect(&rect, point) != FALSE;
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

RECT ThemeButtonRect() {
    return MakeRect(kWindowWidth - 92, 8, kWindowWidth - 64, 31);
}

RECT MenuButtonRect() {
    return MakeRect(kWindowWidth - 62, 8, kWindowWidth - 34, 31);
}

RECT CloseButtonRect() {
    return MakeRect(kWindowWidth - 32, 8, kWindowWidth - 8, 31);
}

void DrawThemeButton(HDC hdc, const RECT& rect, const Palette& p) {
    COLORREF moon = g_theme == ThemeDark ? RGB(206, 212, 224) : RGB(88, 99, 115);
    HBRUSH moonBrush = CreateSolidBrush(moon);
    HBRUSH cutBrush = CreateSolidBrush(p.header);
    HPEN pen = CreatePen(PS_SOLID, 1, moon);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, moonBrush);

    int left = rect.left + 9;
    int top = rect.top + 6;
    Ellipse(hdc, left, top, left + 11, top + 11);
    SelectObject(hdc, cutBrush);
    Ellipse(hdc, left + 4, top - 1, left + 14, top + 10);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    DeleteObject(cutBrush);
    DeleteObject(moonBrush);
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
    RECT button = MakeRect(26, 320, kWindowWidth - 26, 354);
    FillRound(hdc, button, 5, p.button, p.buttonBorder);

    bool unmet = HasUnmetRequirement();
    COLORREF textColor = unmet ? p.disabledText : p.green;
    const wchar_t* text = unmet ? L"Requisito nao atendido" : L"Protecao ativa";

    RECT icon = MakeRect(button.left + 70, button.top + 9, button.left + 89, button.bottom - 8);
    HPEN pen = CreatePen(PS_SOLID, 2, textColor);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, icon.left + 2, icon.top + 2, nullptr);
    LineTo(hdc, icon.left + 8, icon.top + 15);
    LineTo(hdc, icon.left + 16, icon.top + 2);
    MoveToEx(hdc, icon.left + 5, icon.top + 2, nullptr);
    LineTo(hdc, icon.left + 8, icon.top + 8);
    LineTo(hdc, icon.left + 12, icon.top + 2);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    RECT label = MakeRect(button.left + 96, button.top, button.right - 20, button.bottom);
    DrawTextLine(hdc, label, text, font, textColor, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

void DrawTooltip(HDC hdc, HFONT font, const Palette& p) {
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
    DrawTooltip(memory, detailFont, p);
    DrawFooterButton(memory, footerFont, p);

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
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = kTrayMessage;
    g_tray.hIcon = g_iconSmall ? g_iconSmall : LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_NEXORA));
    StringCchCopyW(g_tray.szTip, ARRAYSIZE(g_tray.szTip), L"Nexora Vanguard - protecao ativa");

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

void ShowMainWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }
    SetForegroundWindow(hwnd);
}

void ToggleTheme(HWND hwnd) {
    g_theme = (g_theme == ThemeLight) ? ThemeDark : ThemeLight;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void SaveLogs(HWND hwnd) {
    wchar_t path[MAX_PATH] = {};
    if (!GetTempPathW(ARRAYSIZE(path), path)) {
        MessageBoxW(hwnd, L"Nao foi possivel localizar a pasta temporaria.", kWindowTitle, MB_ICONERROR | MB_OK);
        return;
    }

    StringCchCatW(path, ARRAYSIZE(path), L"NexoraVanguard.log");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        MessageBoxW(hwnd, L"Nao foi possivel salvar os logs.", kWindowTitle, MB_ICONERROR | MB_OK);
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
    MessageBoxW(hwnd, message, kWindowTitle, MB_ICONINFORMATION | MB_OK);
}

void ConfirmExit(HWND hwnd) {
    int result = MessageBoxW(
        hwnd,
        L"Sair do Nexora Vanguard com o Aion aberto ira fechar o jogo.\n\nDeseja fechar o jogo agora?",
        L"Sair do Vanguard",
        MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);

    if (result == IDYES) {
        RemoveTrayIcon();
        ExitProcess(0);
    }
}

void RunPrecheck(HWND hwnd) {
    RefreshRequirements();
    InvalidateRect(hwnd, nullptr, FALSE);
    MessageBoxW(hwnd, L"Verificacao previa concluida.", kWindowTitle, MB_ICONINFORMATION | MB_OK);
}

void ShowMenu(HWND hwnd, POINT screenPoint) {
    HMENU language = CreatePopupMenu();
    AppendMenuW(language, MF_STRING | MF_CHECKED, IDM_LANG_PTBR, L"Portugues (Brasil)");
    AppendMenuW(language, MF_STRING | MF_GRAYED, 0, L"English");
    AppendMenuW(language, MF_STRING | MF_GRAYED, 0, L"Espanol");

    HMENU advanced = CreatePopupMenu();
    AppendMenuW(advanced, MF_STRING | MF_GRAYED, 0, L"Iniciar com o Windows");
    AppendMenuW(advanced, MF_STRING | MF_GRAYED, 0, L"Modo silencioso");

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SAVE_LOGS, L"Salvar logs...");
    AppendMenuW(menu, MF_STRING, IDM_PRECHECK, L"Verificacao previa do Vanguard");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(language), L"Portugues (Brasil)");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(advanced), L"Avancado");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_THEME, g_theme == ThemeLight ? L"Tema escuro" : L"Tema claro");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Sair do Vanguard");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
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
    if (nextHover != g_hoverRow) {
        g_hoverRow = nextHover;
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

    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        UpdateHover(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSELEAVE:
        g_trackingMouse = false;
        if (g_hoverRow != -1) {
            g_hoverRow = -1;
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
            ShowWindow(hwnd, SW_HIDE);
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
            ShowMainWindow(hwnd);
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
        if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT) {
            ShowMainWindow(hwnd);
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
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
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
    ShowWindow(g_window, SW_SHOWNORMAL);
    UpdateWindow(g_window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_iconLarge) {
        DestroyIcon(g_iconLarge);
        g_iconLarge = nullptr;
    }
    if (g_iconSmall) {
        DestroyIcon(g_iconSmall);
        g_iconSmall = nullptr;
    }

    return 0;
}

void StartNexoraVanguard(HINSTANCE instance) {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        return;
    }

    HANDLE thread = CreateThread(nullptr, 0, UiThread, instance, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_started, 0);
    }
}

} // namespace

extern "C" __declspec(dllexport) void WINAPI StartNexoraVanguardUi() {
    StartNexoraVanguard(g_instance ? g_instance : GetModuleHandleW(L"NexoraVanguard.dll"));
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        g_instance = instance;
        StartNexoraVanguard(instance);
    }

    return TRUE;
}
