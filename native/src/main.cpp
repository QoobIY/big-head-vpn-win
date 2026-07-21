#include "model.h"
#include "hysteria_client.h"
#include "subscription.h"
#include "socks_server.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>

namespace {
constexpr COLORREF BACKGROUND = RGB(49, 51, 56);
constexpr COLORREF SIDEBAR = RGB(30, 31, 34);
constexpr COLORREF CARD = RGB(43, 45, 49);
constexpr COLORREF INPUT = RGB(30, 31, 34);
constexpr COLORREF TEXT = RGB(242, 243, 245);
constexpr COLORREF MUTED = RGB(181, 186, 193);
constexpr COLORREF ACCENT = RGB(88, 101, 242);
constexpr COLORREF ERROR_RED = RGB(218, 55, 60);

enum ControlId {
    ID_NAV_HOME = 100, ID_NAV_SERVERS, ID_NAV_SETTINGS,
    ID_CONNECT, ID_GROUPS, ID_PROFILES, ID_URL, ID_ADD_SUBSCRIPTION,
    ID_UPDATE_GROUP, ID_DELETE_GROUP, ID_LOG, ID_COPY_LOG, ID_CLEAR_LOG,
    ID_ADDRESS, ID_PORT
};
constexpr UINT WM_SUBSCRIPTION_READY = WM_APP + 10;
constexpr UINT WM_CONNECT_READY = WM_APP + 11;

struct DownloadPayload {
    SubscriptionResult result;
    std::wstring url;
    std::wstring existingGroupId;
};

struct ConnectPayload {
    HysteriaConnectResult result;
    std::unique_ptr<HysteriaClient> session;
};

struct App {
    HWND window{};
    HWND banner{}, status{}, connect{}, groups{}, profiles{}, url{}, add{}, update{}, remove{};
    HWND log{}, address{}, port{};
    HFONT regular{}, medium{}, title{};
    HBRUSH background{}, sidebar{}, card{}, input{};
    AppModel model;
    bool busy{};
    bool connecting{};
    std::unique_ptr<HysteriaClient> session;
    std::unique_ptr<SocksServer> socks;
};

App app;

std::wstring windowText(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (length) GetWindowTextW(control, result.data(), length + 1);
    return result;
}

void setFont(HWND control, HFONT font) { SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); }

HWND control(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND value = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 10, 10, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    setFont(value, app.regular);
    return value;
}

HWND button(const wchar_t* text, int id) {
    return control(L"BUTTON", text, BS_OWNERDRAW, id);
}

void showBanner(const std::wstring& message, bool error = false) {
    SetWindowTextW(app.banner, message.c_str());
    SetWindowLongPtrW(app.banner, GWLP_USERDATA, error ? 1 : 0);
    ShowWindow(app.banner, message.empty() ? SW_HIDE : SW_SHOW);
    InvalidateRect(app.banner, nullptr, TRUE);
}

void appendLog(const std::wstring& message) {
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t prefix[32]{};
    swprintf(prefix, std::size(prefix), L"%02u:%02u:%02u  ", now.wHour, now.wMinute, now.wSecond);
    std::wstring line(prefix); line += message; line += L"\r\n";
    int length = GetWindowTextLengthW(app.log);
    SendMessageW(app.log, EM_SETSEL, length, length);
    SendMessageW(app.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(app.log, EM_SCROLLCARET, 0, 0);
    if (GetWindowTextLengthW(app.log) > 100000) {
        SendMessageW(app.log, EM_SETSEL, 0, 20000);
        SendMessageW(app.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
}

void refillProfiles() {
    SendMessageW(app.profiles, LB_RESETCONTENT, 0, 0);
    int selectedGroup = static_cast<int>(SendMessageW(app.groups, CB_GETCURSEL, 0, 0));
    std::wstring groupId;
    if (selectedGroup > 0 && static_cast<size_t>(selectedGroup - 1) < app.model.groups.size())
        groupId = app.model.groups[static_cast<size_t>(selectedGroup - 1)].id;
    int selected = -1;
    for (size_t i = 0; i < app.model.profiles.size(); ++i) {
        const auto& profile = app.model.profiles[i];
        if (!groupId.empty() && profile.groupId != groupId) continue;
        int row = static_cast<int>(SendMessageW(app.profiles, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(profile.name.c_str())));
        SendMessageW(app.profiles, LB_SETITEMDATA, row, static_cast<LPARAM>(i));
        if (profile.id == app.model.selectedProfileId) selected = row;
    }
    if (selected < 0 && !app.model.profiles.empty()) selected = 0;
    if (selected >= 0) {
        SendMessageW(app.profiles, LB_SETCURSEL, selected, 0);
        size_t index = static_cast<size_t>(SendMessageW(app.profiles, LB_GETITEMDATA, selected, 0));
        app.model.selectedProfileId = app.model.profiles[index].id;
        SetWindowTextW(app.status, app.model.profiles[index].name.c_str());
    } else SetWindowTextW(app.status, L"Добавьте подписку и выберите сервер");
}

void refillGroups() {
    SendMessageW(app.groups, CB_RESETCONTENT, 0, 0);
    SendMessageW(app.groups, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Все серверы"));
    for (const auto& group : app.model.groups) SendMessageW(app.groups, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(group.name.c_str()));
    SendMessageW(app.groups, CB_SETCURSEL, 0, 0);
    refillProfiles();
}

void beginDownload(const std::wstring& url, const std::wstring& existingGroupId) {
    if (app.busy) return;
    if (url.empty()) { showBanner(L"Введите ссылку на подписку", true); return; }
    app.busy = true;
    EnableWindow(app.add, FALSE); EnableWindow(app.update, FALSE);
    showBanner(L"Загружаю подписку…");
    appendLog(L"Загрузка подписки");
    HWND target = app.window;
    std::thread([target, url, existingGroupId] {
        auto payload = std::make_unique<DownloadPayload>();
        payload->url = url; payload->existingGroupId = existingGroupId;
        payload->result = downloadSubscription(url);
        PostMessageW(target, WM_SUBSCRIPTION_READY, 0, reinterpret_cast<LPARAM>(payload.release()));
    }).detach();
}

void beginConnect(const std::wstring& uri) {
    if (app.connecting || app.session) return;
    app.connecting = true;
    EnableWindow(app.connect, FALSE);
    SetWindowTextW(app.connect, L"Подключение…");
    showBanner(L"Выполняю QUIC/TLS и HTTP/3 авторизацию…");
    appendLog(L"Запуск Hysteria2 handshake");
    HWND target = app.window;
    std::thread([target, uri] {
        auto payload = std::make_unique<ConnectPayload>();
        payload->session = HysteriaClient::connect(uri, payload->result);
        PostMessageW(target, WM_CONNECT_READY, 0, reinterpret_cast<LPARAM>(payload.release()));
    }).detach();
}

void saveListenerFields() {
    auto address = windowText(app.address);
    auto portText = windowText(app.port);
    wchar_t* end = nullptr;
    unsigned long port = wcstoul(portText.c_str(), &end, 10);
    if (address.empty() || !end || *end || port < 1 || port > 65535) {
        showBanner(L"Проверьте адрес и порт локального сервера", true);
        return;
    }
    app.model.listenAddress = address;
    app.model.listenPort = static_cast<unsigned short>(port);
    app.model.save();
}

void layout(int width, int height) {
    int side = 190, margin = 24, top = 78;
    MoveWindow(app.banner, side, 0, width - side, 38, TRUE);
    HWND home = GetDlgItem(app.window, ID_NAV_HOME), servers = GetDlgItem(app.window, ID_NAV_SERVERS), settings = GetDlgItem(app.window, ID_NAV_SETTINGS);
    MoveWindow(home, 16, 82, side - 32, 42, TRUE); MoveWindow(servers, 16, 132, side - 32, 42, TRUE); MoveWindow(settings, 16, 182, side - 32, 42, TRUE);
    int contentX = side + margin, contentW = width - contentX - margin;
    int leftW = std::max(360, contentW * 55 / 100), gap = 18, rightX = contentX + leftW + gap, rightW = contentW - leftW - gap;
    MoveWindow(app.status, contentX + 18, top + 16, leftW - 36, 30, TRUE);
    MoveWindow(app.connect, contentX + 18, top + 56, leftW - 36, 44, TRUE);
    MoveWindow(app.groups, contentX + 18, top + 132, leftW - 36, 36, TRUE);
    int listH = std::max(120, height - top - 340);
    MoveWindow(app.profiles, contentX + 18, top + 180, leftW - 36, listH, TRUE);
    MoveWindow(app.url, contentX + 18, top + 192 + listH, leftW - 154, 36, TRUE);
    MoveWindow(app.add, contentX + leftW - 126, top + 192 + listH, 108, 36, TRUE);
    MoveWindow(app.update, contentX + 18, top + 240 + listH, 122, 34, TRUE);
    MoveWindow(app.remove, contentX + 150, top + 240 + listH, 122, 34, TRUE);
    MoveWindow(app.address, rightX + 18, top + 16, std::max(100, rightW - 142), 36, TRUE);
    MoveWindow(app.port, rightX + rightW - 112, top + 16, 94, 36, TRUE);
    MoveWindow(app.log, rightX + 18, top + 70, rightW - 36, std::max(140, height - top - 148), TRUE);
    MoveWindow(GetDlgItem(app.window, ID_COPY_LOG), rightX + 18, height - 62, 112, 34, TRUE);
    MoveWindow(GetDlgItem(app.window, ID_CLEAR_LOG), rightX + 140, height - 62, 112, 34, TRUE);
}

void paint(HDC dc, const RECT& client) {
    FillRect(dc, &client, app.background);
    RECT side{0, 0, 190, client.bottom}; FillRect(dc, &side, app.sidebar);
    RECT left{214, 78, 214 + std::max(360L, (client.right - 238) * 55L / 100), client.bottom - 18}; FillRect(dc, &left, app.card);
    int rightX = left.right + 18;
    RECT right{rightX, 78, client.right - 24, client.bottom - 18}; FillRect(dc, &right, app.card);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, TEXT);
    SelectObject(dc, app.title);
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (icon) { DrawIconEx(dc, 20, 18, icon, 32, 32, 0, nullptr, DI_NORMAL); DestroyIcon(icon); }
    TextOutW(dc, 62, 24, L"BIG HEAD", 8);
    TextOutW(dc, 214, 48, L"Big Head VPN — Native Preview", 29);
    SelectObject(dc, app.regular); SetTextColor(dc, MUTED);
    TextOutW(dc, rightX + 18, 54, L"Локальный сервер и журнал", 25);
}

void drawButton(const DRAWITEMSTRUCT& item) {
    bool primary = item.CtlID == ID_CONNECT || item.CtlID == ID_ADD_SUBSCRIPTION;
    bool disabled = (item.itemState & ODS_DISABLED) != 0;
    COLORREF color = disabled ? RGB(64, 66, 73) : primary ? ACCENT : RGB(78, 80, 88);
    if ((item.itemState & ODS_SELECTED) && !disabled) color = primary ? RGB(71, 82, 196) : RGB(62, 64, 70);
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    wchar_t label[128]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(148, 151, 158) : TEXT);
    SelectObject(item.hDC, app.medium);
    RECT textRect = item.rcItem;
    DrawTextW(item.hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem; InflateRect(&focus, -3, -3); DrawFocusRect(item.hDC, &focus);
    }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        app.window = hwnd;
        app.background = CreateSolidBrush(BACKGROUND); app.sidebar = CreateSolidBrush(SIDEBAR); app.card = CreateSolidBrush(CARD); app.input = CreateSolidBrush(INPUT);
        app.regular = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        app.medium = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        app.title = CreateFontW(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        app.model.load();
        button(L"Главная", ID_NAV_HOME);
        button(L"Серверы", ID_NAV_SERVERS);
        button(L"Настройки", ID_NAV_SETTINGS);
        app.banner = control(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, 0); ShowWindow(app.banner, SW_HIDE);
        app.status = control(L"STATIC", L"Добавьте подписку и выберите сервер", SS_LEFT | SS_CENTERIMAGE, 0); setFont(app.status, app.medium);
        app.connect = button(L"Подключить", ID_CONNECT); setFont(app.connect, app.medium);
        app.groups = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_GROUPS);
        app.profiles = control(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER, ID_PROFILES);
        app.url = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, ID_URL);
        SendMessageW(app.url, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"https://… ссылка подписки"));
        app.add = button(L"Добавить", ID_ADD_SUBSCRIPTION);
        app.update = button(L"Обновить", ID_UPDATE_GROUP);
        app.remove = button(L"Удалить группу", ID_DELETE_GROUP);
        app.address = control(L"EDIT", app.model.listenAddress.c_str(), ES_AUTOHSCROLL | WS_BORDER, ID_ADDRESS);
        app.port = control(L"EDIT", std::to_wstring(app.model.listenPort).c_str(), ES_NUMBER | WS_BORDER, ID_PORT);
        app.log = control(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_BORDER, ID_LOG);
        button(L"Копировать", ID_COPY_LOG);
        button(L"Очистить", ID_CLEAR_LOG);
        refillGroups(); appendLog(L"Нативное приложение запущено");
        showBanner(L"Hysteria2 TCP через SOCKS5 готов; выберите сервер и подключитесь");
        return 0;
    }
    case WM_SIZE: layout(LOWORD(lParam), HIWORD(lParam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); RECT client{}; GetClientRect(hwnd, &client); paint(dc, client); EndPaint(hwnd, &ps); return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam); HWND item = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, item == app.banner ? TEXT : MUTED);
        if (item == app.banner) {
            SetDCBrushColor(dc, GetWindowLongPtrW(app.banner, GWLP_USERDATA) ? ERROR_RED : ACCENT);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        return reinterpret_cast<LRESULT>(app.card);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam); SetTextColor(dc, TEXT); SetBkColor(dc, INPUT); return reinterpret_cast<LRESULT>(app.input);
    }
    case WM_DRAWITEM: drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(wParam), notification = HIWORD(wParam);
        if (id == ID_ADD_SUBSCRIPTION) beginDownload(windowText(app.url), L"");
        else if (id == ID_UPDATE_GROUP) {
            int selected = static_cast<int>(SendMessageW(app.groups, CB_GETCURSEL, 0, 0));
            if (selected <= 0 || static_cast<size_t>(selected - 1) >= app.model.groups.size()) showBanner(L"Выберите конкретную группу для обновления", true);
            else { const auto& group = app.model.groups[static_cast<size_t>(selected - 1)]; beginDownload(group.url, group.id); }
        } else if (id == ID_DELETE_GROUP) {
            int selected = static_cast<int>(SendMessageW(app.groups, CB_GETCURSEL, 0, 0));
            if (selected <= 0 || static_cast<size_t>(selected - 1) >= app.model.groups.size()) showBanner(L"Выберите группу для удаления", true);
            else { auto name = app.model.groups[static_cast<size_t>(selected - 1)].name; app.model.deleteGroup(app.model.groups[static_cast<size_t>(selected - 1)].id); app.model.save(); refillGroups(); appendLog(L"Удалена группа: " + name); showBanner(L"Группа удалена"); }
        } else if (id == ID_PROFILES && notification == LBN_SELCHANGE) {
            int selected = static_cast<int>(SendMessageW(app.profiles, LB_GETCURSEL, 0, 0));
            if (selected >= 0) {
                size_t index = static_cast<size_t>(SendMessageW(app.profiles, LB_GETITEMDATA, selected, 0));
                app.model.selectedProfileId = app.model.profiles[index].id; app.model.save(); SetWindowTextW(app.status, app.model.profiles[index].name.c_str());
                if (app.session) {
                    app.socks.reset(); app.session.reset(); SetWindowTextW(app.connect, L"Подключить");
                    appendLog(L"Старый туннель остановлен при смене сервера");
                    const auto& uri = app.model.profiles[index].uri;
                    if (uri.rfind(L"hysteria2://", 0) == 0 || uri.rfind(L"hy2://", 0) == 0) beginConnect(uri);
                    else showBanner(L"Новый профиль не Hysteria2; туннель отключён", true);
                }
            }
        } else if (id == ID_GROUPS && notification == CBN_SELCHANGE) {
            refillProfiles();
        } else if (id == ID_CONNECT) {
            saveListenerFields();
            if (app.session) {
                SetWindowTextW(app.connect, L"Отключение…"); EnableWindow(app.connect, FALSE);
                app.socks.reset();
                app.session.reset();
                SetWindowTextW(app.connect, L"Подключить"); EnableWindow(app.connect, TRUE);
                showBanner(L"Hysteria2 отключена"); appendLog(L"Соединение остановлено");
                return 0;
            }
            auto selected = std::find_if(app.model.profiles.begin(), app.model.profiles.end(), [&](const auto& item) { return item.id == app.model.selectedProfileId; });
            if (selected == app.model.profiles.end()) {
                showBanner(L"Сначала выберите Hysteria2-сервер", true);
            } else if (selected->uri.rfind(L"hysteria2://", 0) != 0 && selected->uri.rfind(L"hy2://", 0) != 0) {
                showBanner(L"Первая нативная версия подключает только Hysteria2", true);
                appendLog(L"Профиль пропущен: VLESS будет добавлен после Hysteria2");
            } else {
                beginConnect(selected->uri);
            }
        } else if (id == ID_COPY_LOG) {
            SendMessageW(app.log, EM_SETSEL, 0, -1); SendMessageW(app.log, WM_COPY, 0, 0); SendMessageW(app.log, EM_SETSEL, -1, -1);
            showBanner(L"Журнал скопирован");
        } else if (id == ID_CLEAR_LOG) SetWindowTextW(app.log, L"");
        return 0;
    }
    case WM_SUBSCRIPTION_READY: {
        std::unique_ptr<DownloadPayload> payload(reinterpret_cast<DownloadPayload*>(lParam));
        app.busy = false; EnableWindow(app.add, TRUE); EnableWindow(app.update, TRUE);
        if (!payload->result.error.empty()) {
            showBanner(payload->result.error, true); appendLog(L"Ошибка: " + payload->result.error); return 0;
        }
        SubscriptionGroup group;
        if (!payload->existingGroupId.empty()) {
            auto found = std::find_if(app.model.groups.begin(), app.model.groups.end(), [&](const auto& item) { return item.id == payload->existingGroupId; });
            if (found != app.model.groups.end()) group = *found;
        }
        if (group.id.empty()) group = {newId(), payload->result.suggestedName.empty() ? L"Подписка" : payload->result.suggestedName, payload->url};
        size_t imported = payload->result.profiles.size();
        app.model.replaceGroup(group, std::move(payload->result.profiles)); app.model.save(); refillGroups();
        showBanner(L"Подписка сохранена: " + std::to_wstring(imported) + L" серверов");
        appendLog(L"Подписка обновлена: " + group.name); SetWindowTextW(app.url, L""); return 0;
    }
    case WM_CONNECT_READY: {
        std::unique_ptr<ConnectPayload> payload(reinterpret_cast<ConnectPayload*>(lParam));
        app.connecting = false; EnableWindow(app.connect, TRUE);
        if (payload->session && payload->result.connected) {
            app.session = std::move(payload->session);
            std::wstring listenerError;
            app.socks = SocksServer::start(app.model.listenAddress, app.model.listenPort, *app.session, listenerError);
            if (!app.socks) {
                app.session.reset();
                SetWindowTextW(app.connect, L"Подключить");
                showBanner(listenerError, true); appendLog(L"Ошибка SOCKS5: " + listenerError);
                return 0;
            }
            SetWindowTextW(app.connect, L"Отключить");
            std::wstring ready = L"SOCKS5 работает на " + app.model.listenAddress + L":" + std::to_wstring(app.model.listenPort);
            showBanner(ready); appendLog(payload->result.message); appendLog(ready);
        } else {
            SetWindowTextW(app.connect, L"Подключить");
            showBanner(payload->result.message.empty() ? L"Hysteria2 не подключилась" : payload->result.message, true);
            appendLog(L"Ошибка подключения: " + payload->result.message);
        }
        return 0;
    }
    case WM_CLOSE: saveListenerFields(); DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        app.socks.reset();
        app.session.reset();
        DeleteObject(app.regular); DeleteObject(app.medium); DeleteObject(app.title); DeleteObject(app.background); DeleteObject(app.sidebar); DeleteObject(app.card); DeleteObject(app.input);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW; cls.lpfnWndProc = windowProc; cls.hInstance = instance;
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); cls.hIconSm = cls.hIcon;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = nullptr; cls.lpszClassName = L"BigHeadVPNNativeWindow";
    if (!RegisterClassExW(&cls)) return 1;
    BOOL dark = TRUE;
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"Big Head VPN", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, nullptr, nullptr, instance, nullptr);
    if (!window) return 2;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    ShowWindow(window, show); UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return static_cast<int>(message.wParam);
}
