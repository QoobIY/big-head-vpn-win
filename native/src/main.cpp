#include "model.h"
#include "tunnel_client.h"
#include "subscription.h"
#include "socks_server.h"
#include "process_filter.h"
#include "autostart.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>

namespace {
constexpr COLORREF BACKGROUND = RGB(18, 20, 25);
constexpr COLORREF CARD = RGB(25, 28, 35);
constexpr COLORREF INPUT = RGB(15, 17, 22);
constexpr COLORREF BORDER = RGB(43, 47, 58);
constexpr COLORREF TEXT = RGB(246, 247, 251);
constexpr COLORREF MUTED = RGB(157, 164, 178);
constexpr COLORREF ACCENT = RGB(88, 101, 242);
constexpr COLORREF ERROR_RED = RGB(218, 55, 60);

enum ControlId {
    ID_CONNECT = 100, ID_GROUPS, ID_PROFILES, ID_URL, ID_ADD_SUBSCRIPTION,
    ID_UPDATE_GROUP, ID_DELETE_GROUP, ID_LOG, ID_COPY_LOG, ID_CLEAR_LOG,
    ID_ADDRESS, ID_PORT, ID_RUNNING_PROCESSES, ID_SELECTED_PROCESSES,
    ID_REFRESH_PROCESSES, ID_ADD_PROCESS, ID_REMOVE_PROCESS, ID_AUTOSTART, ID_PROCESS_ROUTING, ID_DELETE_PROFILE,
    ID_TRAY_OPEN = 200, ID_TRAY_TOGGLE, ID_TRAY_EXIT
};
constexpr UINT WM_SUBSCRIPTION_READY = WM_APP + 10;
constexpr UINT WM_CONNECT_READY = WM_APP + 11;
constexpr UINT WM_TRAY = WM_APP + 12;
constexpr UINT WM_TUNNEL_ERROR = WM_APP + 13;
constexpr UINT WM_DISCONNECT_READY = WM_APP + 14;
constexpr UINT_PTR FILTER_STATUS_TIMER = 1;
constexpr int IDR_MANROPE = 101;
constexpr UINT TRAY_ICON_ID = 1;

UINT taskbarCreatedMessage{};

struct DownloadPayload {
    SubscriptionResult result;
    std::wstring url;
    std::wstring existingGroupId;
};

struct ConnectPayload {
    TunnelConnectResult result;
    std::unique_ptr<TunnelClient> session;
};

struct DisconnectPayload {
    std::wstring reconnectProfileId;
};

struct App {
    HWND window{};
    HWND banner{}, status{}, connect{}, groups{}, profiles{}, url{}, add{}, update{}, remove{};
    HWND log{}, address{}, port{};
    HWND runningProcesses{}, selectedProcesses{}, refreshProcesses{}, addProcess{}, removeProcess{};
    HWND filterStatus{}, autostart{}, processRouting{}, removeProfile{};
    HFONT regular{}, medium{}, title{}, small{};
    HBRUSH background{}, card{}, input{};
    HANDLE fontResource{};
    AppModel model;
    std::wstring activeProfileId;
    bool busy{};
    bool connecting{};
    bool disconnecting{};
    bool autostartEnabled{};
    bool trayAdded{};
    bool exiting{};
    HICON trayStatusIcon{};
    std::unique_ptr<TunnelClient> session;
    std::unique_ptr<SocksServer> socks;
    std::unique_ptr<ProcessFilter> processFilter;
    unsigned long long lastMatched{}, lastRedirected{}, lastProxyReplies{}, lastAccepted{}, lastAcceptAttempts{}, lastNatMisses{}, lastUdpMatched{}, lastUdpSent{}, lastUdpReceived{}, lastInjectionFailures{};
};

App app;

void updateTrayIcon();

HICON createTrayStatusIcon(COLORREF statusColor) {
    const int width = std::max(16, GetSystemMetrics(SM_CXSMICON));
    const int height = std::max(16, GetSystemMetrics(SM_CYSMICON));
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = width;
    header.bV5Height = -height;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;

    HDC screen = GetDC(nullptr);
    void* rawPixels{};
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    HBITMAP mask = CreateBitmap(width, height, 1, 1, nullptr);
    HDC canvas = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!color || !mask || !canvas || !rawPixels) {
        if (canvas) DeleteDC(canvas);
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        return nullptr;
    }

    HGDIOBJ previous = SelectObject(canvas, color);
    HICON base = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(1), IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
    if (base) {
        DrawIconEx(canvas, 0, 0, base, width, height, 0, nullptr, DI_NORMAL);
        DestroyIcon(base);
    }

    auto* pixels = static_cast<uint32_t*>(rawPixels);
    const int radius = std::max(3, width / 5);
    const int centerX = width - radius;
    const int centerY = height - radius;
    const int borderRadiusSquared = radius * radius;
    const int fillRadius = std::max(1, radius - 2);
    const int fillRadiusSquared = fillRadius * fillRadius;
    const uint32_t fill = 0xff000000U | (static_cast<uint32_t>(GetRValue(statusColor)) << 16U) |
        (static_cast<uint32_t>(GetGValue(statusColor)) << 8U) | GetBValue(statusColor);
    for (int y = centerY - radius; y <= centerY + radius; ++y) {
        for (int x = centerX - radius; x <= centerX + radius; ++x) {
            if (x < 0 || y < 0 || x >= width || y >= height) continue;
            const int distance = (x - centerX) * (x - centerX) + (y - centerY) * (y - centerY);
            if (distance <= borderRadiusSquared)
                pixels[y * width + x] = distance <= fillRadiusSquared ? fill : 0xff111318U;
        }
    }

    SelectObject(canvas, previous);
    DeleteDC(canvas);
    ICONINFO info{};
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON result = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return result;
}

void restoreWindow() {
    if (IsIconic(app.window)) ShowWindow(app.window, SW_RESTORE);
    else ShowWindow(app.window, SW_SHOW);
    SetForegroundWindow(app.window);
}

void removeTrayIcon() {
    if (app.trayAdded) {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = app.window;
        icon.uID = TRAY_ICON_ID;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        app.trayAdded = false;
    }
    if (app.trayStatusIcon) {
        DestroyIcon(app.trayStatusIcon);
        app.trayStatusIcon = nullptr;
    }
}

void updateTrayIcon() {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app.window;
    icon.uID = TRAY_ICON_ID;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = WM_TRAY;
    HICON previousIcon = app.trayStatusIcon;
    const COLORREF statusColor = (app.connecting || app.disconnecting) ? RGB(245, 158, 11)
        : app.session ? RGB(34, 197, 94) : RGB(113, 119, 132);
    app.trayStatusIcon = createTrayStatusIcon(statusColor);
    icon.hIcon = app.trayStatusIcon ? app.trayStatusIcon
                                   : LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    const wchar_t* tip = app.disconnecting ? L"Big Head VPN — отключение…"
        : app.connecting ? L"Big Head VPN — подключение…"
        : app.session ? L"Big Head VPN — подключено" : L"Big Head VPN — отключено";
    wcsncpy_s(icon.szTip, tip, _TRUNCATE);
    if (!app.trayAdded) {
        app.trayAdded = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
        if (app.trayAdded) {
            icon.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &icon);
        }
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &icon);
    }
    if (previousIcon) DestroyIcon(previousIcon);
}

void showTrayMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, ID_TRAY_OPEN, L"Открыть Big Head VPN");
    AppendMenuW(menu, MF_STRING | (app.disconnecting ? MF_GRAYED : 0), ID_TRAY_TOGGLE,
        app.disconnecting ? L"Отключение VPN…" : app.session ? L"Отключить VPN" : L"Подключить VPN");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Выход");
    SetForegroundWindow(app.window);
    UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        point.x, point.y, 0, app.window, nullptr);
    DestroyMenu(menu);
    if (command) PostMessageW(app.window, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

std::wstring windowText(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (length) GetWindowTextW(control, result.data(), length + 1);
    return result;
}

void setFont(HWND control, HFONT font) { SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); }

void updateAutostartButton() {
    if (!app.autostart) return;
    SetWindowTextW(app.autostart, app.autostartEnabled ? L"● Автозагрузка включена" : L"○ Автозагрузка выключена");
    InvalidateRect(app.autostart, nullptr, TRUE);
}

HWND control(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND value = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 10, 10, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    setFont(value, app.regular);
    SetWindowTheme(value, L"DarkMode_Explorer", nullptr);
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
    if (selected < 0 && SendMessageW(app.profiles, LB_GETCOUNT, 0, 0) > 0) selected = 0;
    if (selected >= 0) {
        SendMessageW(app.profiles, LB_SETCURSEL, selected, 0);
        size_t index = static_cast<size_t>(SendMessageW(app.profiles, LB_GETITEMDATA, selected, 0));
        app.model.selectedProfileId = app.model.profiles[index].id;
        SetWindowTextW(app.status, app.model.profiles[index].name.c_str());
    } else {
        app.model.selectedProfileId.clear();
        SetWindowTextW(app.status, L"Добавьте подписку или отдельный сервер");
    }
    EnableWindow(app.removeProfile, selected >= 0);
    EnableWindow(app.remove, selectedGroup > 0);
    EnableWindow(app.update, selectedGroup > 0);
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

void addSubscriptionOrProfile(const std::wstring& input) {
    std::wstring uri = normalizeProfileUri(input);
    if (!supportedProfile(uri)) {
        beginDownload(input, L"");
        return;
    }

    auto duplicate = std::find_if(app.model.profiles.begin(), app.model.profiles.end(),
        [&](const auto& profile) { return profile.uri == uri; });
    if (duplicate != app.model.profiles.end()) {
        app.model.selectedProfileId = duplicate->id;
        app.model.save();
        refillGroups();
        SetWindowTextW(app.url, L"");
        showBanner(L"Этот сервер уже добавлен");
        return;
    }

    Profile profile{newId(), L"", profileName(uri), std::move(uri)};
    const std::wstring name = profile.name;
    app.model.selectedProfileId = profile.id;
    app.model.profiles.push_back(std::move(profile));
    app.model.save();
    refillGroups();
    SetWindowTextW(app.url, L"");
    if (profileKind(app.model.profiles.back().uri) == ProfileKind::Unsupported)
        showBanner(L"Сервер добавлен, но его транспорт пока не поддерживается", true);
    else
        showBanner(L"Сервер добавлен: " + name);
    appendLog(L"Добавлен отдельный сервер: " + name);
}

void beginConnect(const std::wstring& profileId) {
    if (app.connecting || app.disconnecting || app.session) return;
    auto selected = std::find_if(app.model.profiles.begin(), app.model.profiles.end(),
        [&](const auto& profile) { return profile.id == profileId; });
    if (selected == app.model.profiles.end()) {
        showBanner(L"Выбранный сервер больше не существует", true);
        return;
    }
    app.activeProfileId = selected->id;
    const ProfileKind kind = profileKind(selected->uri);
    app.connecting = true;
    EnableWindow(app.connect, FALSE);
    SetWindowTextW(app.connect, L"Подключение…");
    updateTrayIcon();
    switch (kind) {
    case ProfileKind::VlessXhttpTls:
        showBanner(L"Проверяю VLESS XHTTP/TLS…");
        break;
    case ProfileKind::Hysteria2:
        showBanner(L"Выполняю QUIC/TLS и HTTP/3 авторизацию…");
        break;
    case ProfileKind::VlessGrpcTls:
        showBanner(L"VLESS gRPC/TLS ещё не включён в эту сборку", true);
        break;
    case ProfileKind::VlessTcpReality:
        showBanner(L"Проверяю VLESS TCP/REALITY…");
        break;
    case ProfileKind::VlessVisionReality:
        showBanner(L"Проверяю VLESS TCP/REALITY Vision…");
        break;
    case ProfileKind::VlessGrpcReality:
        showBanner(L"Проверяю VLESS gRPC/REALITY…");
        break;
    default:
        showBanner(L"Формат выбранного сервера пока не поддерживается", true);
        break;
    }
    appendLog(L"Запуск " + profileKindName(kind) + L": " + selected->name);
    HWND target = app.window;
    std::thread([target, uri = selected->uri] {
        auto payload = std::make_unique<ConnectPayload>();
        payload->session = connectTunnel(uri, payload->result);
        PostMessageW(target, WM_CONNECT_READY, 0, reinterpret_cast<LPARAM>(payload.release()));
    }).detach();
}

void beginDisconnect(std::wstring reconnectProfileId = {}) {
    if (app.disconnecting || !app.session) return;
    app.disconnecting = true;
    SetWindowTextW(app.connect, L"Отключение…");
    EnableWindow(app.connect, FALSE);
    EnableWindow(app.profiles, FALSE);
    updateTrayIcon();

    HWND target = app.window;
    std::thread([target, reconnectProfileId = std::move(reconnectProfileId),
                    processFilter = std::move(app.processFilter),
                    socks = std::move(app.socks), session = std::move(app.session)]() mutable {
        // Stop the tunnel first so blocked relay calls wake up, then join the
        // SOCKS/WinDivert workers while the TunnelClient is still alive.
        session->stop();
        processFilter.reset();
        socks.reset();
        session.reset();
        auto payload = std::make_unique<DisconnectPayload>();
        payload->reconnectProfileId = std::move(reconnectProfileId);
        if (PostMessageW(target, WM_DISCONNECT_READY, 0,
                reinterpret_cast<LPARAM>(payload.get())))
            payload.release();
    }).detach();
}

std::wstring lowerName(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

std::vector<std::wstring> runningProcessNames() {
    std::vector<std::wstring> names;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return names;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == GetCurrentProcessId() || !entry.szExeFile[0]) continue;
            std::wstring name = entry.szExeFile;
            auto duplicate = std::find_if(names.begin(), names.end(), [&](const auto& item) { return lowerName(item) == lowerName(name); });
            if (duplicate == names.end()) names.push_back(std::move(name));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    std::sort(names.begin(), names.end(), [](const auto& left, const auto& right) { return lowerName(left) < lowerName(right); });
    return names;
}

void refillSelectedProcesses() {
    SendMessageW(app.selectedProcesses, LB_RESETCONTENT, 0, 0);
    for (const auto& name : app.model.filteredProcesses)
        SendMessageW(app.selectedProcesses, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
}

void refreshRunningProcesses() {
    SendMessageW(app.runningProcesses, LB_RESETCONTENT, 0, 0);
    for (const auto& name : runningProcessNames())
        SendMessageW(app.runningProcesses, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    appendLog(L"Список запущенных приложений обновлён");
}

std::wstring selectedListText(HWND list) {
    int selected = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (selected < 0) return {};
    int length = static_cast<int>(SendMessageW(list, LB_GETTEXTLEN, selected, 0));
    if (length < 0) return {};
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(list, LB_GETTEXT, selected, reinterpret_cast<LPARAM>(value.data()));
    value.resize(static_cast<size_t>(length));
    return value;
}

void updateFilterStatus(bool writeLog = false);

void processSelectionChanged(const std::wstring& message) {
    app.model.save();
    refillSelectedProcesses();
    updateFilterStatus();
    if (app.session && app.model.processRoutingEnabled) showBanner(message + L". Переподключите VPN, чтобы применить список");
    else showBanner(message);
}

void addSelectedProcess() {
    auto name = selectedListText(app.runningProcesses);
    if (name.empty()) { showBanner(L"Выберите приложение в списке запущенных", true); return; }
    auto key = lowerName(name);
    auto found = std::find_if(app.model.filteredProcesses.begin(), app.model.filteredProcesses.end(), [&](const auto& item) { return lowerName(item) == key; });
    if (found == app.model.filteredProcesses.end()) app.model.filteredProcesses.push_back(name);
    processSelectionChanged(name + L" добавлен в VPN");
}

void removeSelectedProcess() {
    auto name = selectedListText(app.selectedProcesses);
    if (name.empty()) { showBanner(L"Выберите приложение в списке VPN", true); return; }
    auto key = lowerName(name);
    std::erase_if(app.model.filteredProcesses, [&](const auto& item) { return lowerName(item) == key; });
    processSelectionChanged(name + L" убран из VPN");
}

void updateFilterStatus(bool writeLog) {
    if (!app.processFilter) {
        SetWindowTextW(app.filterStatus, !app.model.processRoutingEnabled
            ? L"Выключено — работает только локальный прокси"
            : app.model.filteredProcesses.empty()
            ? L"Не выбрано — доступен только локальный SOCKS5"
            : L"Список сохранён — фильтр запустится при подключении");
        return;
    }
    auto matched = app.processFilter->matchedFlows();
    auto redirected = app.processFilter->redirectedPackets();
    auto proxyReplies = app.processFilter->proxyReplies();
    auto accepted = app.processFilter->acceptedConnections();
    auto attempts = app.processFilter->acceptAttempts();
    auto natMisses = app.processFilter->natMisses();
    auto udpMatched = app.processFilter->udpMatchedFlows();
    auto udpSent = app.processFilter->udpSentPackets();
    auto udpReceived = app.processFilter->udpReceivedPackets();
    auto tcpLate = app.processFilter->tcpLateFlows();
    auto tcpLateMax = app.processFilter->tcpLateMaxMs();
    auto udpLate = app.processFilter->udpLateFlows();
    auto udpLatePackets = app.processFilter->udpLatePackets();
    auto udpLateMax = app.processFilter->udpLateMaxMs();
    auto udpResponseLast = app.processFilter->udpResponseLastMs();
    auto udpResponseMax = app.processFilter->udpResponseMaxMs();
    auto injectionFailures = app.processFilter->injectionFailures();
    std::wstring status = L"TCP: соединений " + std::to_wstring(matched) +
        L"  •  пакетов ↑" + std::to_wstring(redirected) + L"/↓" + std::to_wstring(proxyReplies) + L"  •  relay " + std::to_wstring(accepted);
    if (attempts != accepted) status += L"/" + std::to_wstring(attempts) + L" (NAT miss " + std::to_wstring(natMisses) + L")";
    if (injectionFailures) status += L"  •  send errors " + std::to_wstring(injectionFailures) + L"/" + std::to_wstring(app.processFilter->lastInjectionError());
    status += L"  •  UDP " + std::to_wstring(udpMatched) + L":" + std::to_wstring(udpSent) + L"/" + std::to_wstring(udpReceived);
    status += L"  •  DIAG late TCP " + std::to_wstring(tcpLate) + L"/" + std::to_wstring(tcpLateMax) + L"ms";
    status += L" UDP " + std::to_wstring(udpLate) + L":" + std::to_wstring(udpLatePackets) + L"/" + std::to_wstring(udpLateMax) + L"ms";
    status += L" reply " + std::to_wstring(udpResponseLast) + L"/" + std::to_wstring(udpResponseMax) + L"ms";
    SetWindowTextW(app.filterStatus, status.c_str());
    if (writeLog && (matched != app.lastMatched || redirected != app.lastRedirected || proxyReplies != app.lastProxyReplies || accepted != app.lastAccepted ||
        attempts != app.lastAcceptAttempts || natMisses != app.lastNatMisses || udpMatched != app.lastUdpMatched ||
        udpSent != app.lastUdpSent || udpReceived != app.lastUdpReceived || injectionFailures != app.lastInjectionFailures))
        appendLog(L"Фильтр: " + status);
    app.lastMatched = matched; app.lastRedirected = redirected; app.lastProxyReplies = proxyReplies; app.lastAccepted = accepted;
    app.lastAcceptAttempts = attempts; app.lastNatMisses = natMisses;
    app.lastUdpMatched = udpMatched; app.lastUdpSent = udpSent; app.lastUdpReceived = udpReceived;
    app.lastInjectionFailures = injectionFailures;
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
    int margin = 24, top = 126;
    MoveWindow(app.banner, margin, 68, width - margin * 2, 40, TRUE);
    MoveWindow(app.autostart, width - margin - 246, 20, 246, 36, TRUE);
    int contentX = margin, contentW = width - margin * 2;
    int leftW = std::max(360, contentW * 55 / 100), gap = 18, rightX = contentX + leftW + gap, rightW = contentW - leftW - gap;
    MoveWindow(app.status, contentX + 20, top + 18, leftW - 40, 28, TRUE);
    MoveWindow(app.connect, contentX + 20, top + 56, leftW - 40, 44, TRUE);
    MoveWindow(app.groups, contentX + 20, top + 140, leftW - 40, 240, TRUE);
    int listH = std::max(120, height - top - 376);
    MoveWindow(app.profiles, contentX + 20, top + 190, leftW - 40, listH, TRUE);
    MoveWindow(app.url, contentX + 20, top + 202 + listH, leftW - 158, 38, TRUE);
    MoveWindow(app.add, contentX + leftW - 128, top + 202 + listH, 108, 38, TRUE);
    int actionWidth = (leftW - 56) / 3;
    MoveWindow(app.update, contentX + 20, top + 252 + listH, actionWidth, 36, TRUE);
    MoveWindow(app.removeProfile, contentX + 28 + actionWidth, top + 252 + listH, actionWidth, 36, TRUE);
    MoveWindow(app.remove, contentX + 36 + actionWidth * 2, top + 252 + listH, actionWidth, 36, TRUE);
    MoveWindow(app.address, rightX + 20, top + 30, std::max(100, rightW - 146), 38, TRUE);
    MoveWindow(app.port, rightX + rightW - 114, top + 30, 94, 38, TRUE);
    int processListTop = top + 150;
    int processListHeight = std::clamp((height - top) * 28 / 100, 110, 180);
    int processWidth = (rightW - 50) / 2;
    MoveWindow(app.processRouting, rightX + 20, top + 77, rightW - 40, 26, TRUE);
    MoveWindow(app.refreshProcesses, rightX + rightW - 106, top + 105, 86, 28, TRUE);
    MoveWindow(app.filterStatus, rightX + 20, top + 106, rightW - 136, 24, TRUE);
    MoveWindow(app.runningProcesses, rightX + 20, processListTop, processWidth, processListHeight, TRUE);
    MoveWindow(app.selectedProcesses, rightX + 30 + processWidth, processListTop, processWidth, processListHeight, TRUE);
    MoveWindow(app.addProcess, rightX + 20, processListTop + processListHeight + 8, processWidth, 34, TRUE);
    MoveWindow(app.removeProcess, rightX + 30 + processWidth, processListTop + processListHeight + 8, processWidth, 34, TRUE);
    int logTop = processListTop + processListHeight + 72;
    MoveWindow(app.log, rightX + 20, logTop, rightW - 40, std::max(90, height - logTop - 80), TRUE);
    MoveWindow(GetDlgItem(app.window, ID_COPY_LOG), rightX + 18, height - 62, 112, 34, TRUE);
    MoveWindow(GetDlgItem(app.window, ID_CLEAR_LOG), rightX + 140, height - 62, 112, 34, TRUE);
}

void roundedBox(HDC dc, RECT rect, COLORREF fill, COLORREF outline, int radius = 14) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, outline);
    auto oldBrush = SelectObject(dc, brush); auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}

void label(HDC dc, int x, int y, const wchar_t* text) {
    SelectObject(dc, app.small); SetTextColor(dc, MUTED);
    TextOutW(dc, x, y, text, static_cast<int>(wcslen(text)));
}

void paint(HDC dc, const RECT& client) {
    FillRect(dc, &client, app.background);
    constexpr int margin = 24;
    RECT left{margin, 126, margin + std::max(360L, (client.right - margin * 2L) * 55L / 100), client.bottom - 18}; roundedBox(dc, left, CARD, BORDER);
    int rightX = left.right + 18;
    RECT right{rightX, 126, client.right - 24, client.bottom - 18}; roundedBox(dc, right, CARD, BORDER);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, TEXT);
    SelectObject(dc, app.title);
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 38, 38, LR_DEFAULTCOLOR));
    if (icon) { DrawIconEx(dc, 24, 18, icon, 38, 38, 0, nullptr, DI_NORMAL); DestroyIcon(icon); }
    TextOutW(dc, 72, 19, L"Big Head VPN", 12);
    SelectObject(dc, app.regular); SetTextColor(dc, MUTED);
    constexpr wchar_t subtitle[] = L"Лёгкий нативный клиент для Windows";
    TextOutW(dc, 73, 48, subtitle, static_cast<int>(std::size(subtitle) - 1));
    label(dc, margin + 20, 247, L"СЕРВЕРЫ И ПОДПИСКИ");
    label(dc, rightX + 20, 143, L"ЛОКАЛЬНЫЙ PROXY: SOCKS5 + HTTP");
    label(dc, rightX + 20, 258, L"ЗАПУЩЕННЫЕ");
    int processWidth = (right.right - right.left - 50) / 2;
    label(dc, rightX + 30 + processWidth, 258, L"ВЫБРАННЫЕ");
    int processListHeight = std::clamp(static_cast<int>((client.bottom - 126) * 28 / 100), 110, 180);
    label(dc, rightX + 20, 126 + 150 + processListHeight + 55, L"ЖУРНАЛ СОЕДИНЕНИЯ");
}

void drawButton(const DRAWITEMSTRUCT& item) {
    bool primary = item.CtlID == ID_CONNECT || item.CtlID == ID_ADD_SUBSCRIPTION || item.CtlID == ID_ADD_PROCESS;
    bool disabled = (item.itemState & ODS_DISABLED) != 0;
    bool activeAutostart = item.CtlID == ID_AUTOSTART && app.autostartEnabled;
    COLORREF color = disabled ? RGB(54, 57, 66) : activeAutostart ? RGB(41, 126, 85) : primary ? ACCENT : RGB(47, 51, 61);
    if ((item.itemState & ODS_SELECTED) && !disabled) color = primary ? RGB(71, 82, 196) : RGB(62, 64, 70);
    RECT background = item.rcItem; FillRect(item.hDC, &background, app.card);
    roundedBox(item.hDC, item.rcItem, color, color, 10);
    wchar_t label[128]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(148, 151, 158) : TEXT);
    SelectObject(item.hDC, app.medium);
    RECT textRect = item.rcItem;
    DrawTextW(item.hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem; InflateRect(&focus, -4, -4); DrawFocusRect(item.hDC, &focus);
    }
}

void drawBanner(const DRAWITEMSTRUCT& item) {
    FillRect(item.hDC, &item.rcItem, app.background);
    COLORREF color = GetWindowLongPtrW(app.banner, GWLP_USERDATA) ? ERROR_RED : ACCENT;
    roundedBox(item.hDC, item.rcItem, color, color, 10);
    wchar_t message[1024]{};
    GetWindowTextW(app.banner, message, static_cast<int>(std::size(message)));
    SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, TEXT); SelectObject(item.hDC, app.medium);
    RECT textRect = item.rcItem; textRect.left += 16; textRect.right -= 16;
    DrawTextW(item.hDC, message, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void drawChoice(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)) return;
    const int saved = SaveDC(item.hDC);
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    FillRect(item.hDC, &item.rcItem, app.input);
    RECT row = item.rcItem; InflateRect(&row, -3, -3);
    roundedBox(item.hDC, row, selected ? RGB(39, 51, 73) : RGB(29, 33, 43),
               selected ? RGB(79, 137, 178) : RGB(36, 41, 53), 10);
    SetBkMode(item.hDC, TRANSPARENT);
    if (item.CtlID == ID_PROFILES && item.itemData < app.model.profiles.size()) {
        const auto& profile = app.model.profiles[item.itemData];
        const auto kind = profileKind(profile.uri);
        const bool hysteria = kind == ProfileKind::Hysteria2;
        const bool unsupported = kind == ProfileKind::Unsupported;
        const wchar_t* badge = hysteria ? L"HY2" : unsupported ? L"ДРУГОЙ" : L"VLESS";
        COLORREF tint = hysteria ? RGB(79, 211, 185) : unsupported ? RGB(210, 163, 95) : RGB(142, 170, 255);
        RECT pill{row.right - 82, row.top + 9, row.right - 10, row.top + 31};
        roundedBox(item.hDC, pill, hysteria ? RGB(28, 66, 63) : unsupported ? RGB(66, 51, 33) : RGB(44, 48, 78),
                   selected ? tint : BORDER, 8);
        SelectObject(item.hDC, app.small); SetTextColor(item.hDC, tint);
        DrawTextW(item.hDC, badge, -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT title{row.left + 13, row.top + 6, pill.left - 10, row.top + 31};
        SelectObject(item.hDC, app.medium); SetTextColor(item.hDC, TEXT);
        DrawTextW(item.hDC, profile.name.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        std::wstring detail = profileKindName(kind);
        if (profile.groupId.empty()) detail += L"  ·  Отдельный сервер";
        else {
            const auto group = std::find_if(app.model.groups.begin(), app.model.groups.end(),
                [&](const auto& entry) { return entry.id == profile.groupId; });
            if (group != app.model.groups.end()) detail += L"  ·  " + group->name;
        }
        RECT subtitle{row.left + 13, row.top + 33, row.right - 12, row.bottom - 5};
        SelectObject(item.hDC, app.small); SetTextColor(item.hDC, MUTED);
        DrawTextW(item.hDC, detail.c_str(), -1, &subtitle, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        const bool list = item.CtlType == ODT_LISTBOX;
        const auto length = SendMessageW(item.hwndItem, list ? LB_GETTEXTLEN : CB_GETLBTEXTLEN, item.itemID, 0);
        if (length >= 0) {
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            SendMessageW(item.hwndItem, list ? LB_GETTEXT : CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text.data()));
            SelectObject(item.hDC, selected ? app.medium : app.regular);
            SetTextColor(item.hDC, selected ? TEXT : RGB(211, 215, 224));
            RECT textRect = row; textRect.left += 12; textRect.right -= 8;
            DrawTextW(item.hDC, text.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = row; InflateRect(&focus, -3, -3); DrawFocusRect(item.hDC, &focus);
    }
    RestoreDC(item.hDC, saved);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        app.window = hwnd;
        updateTrayIcon();
        app.background = CreateSolidBrush(BACKGROUND); app.card = CreateSolidBrush(CARD); app.input = CreateSolidBrush(INPUT);
        HRSRC fontInfo = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDR_MANROPE), RT_RCDATA);
        if (fontInfo) {
            HGLOBAL fontData = LoadResource(GetModuleHandleW(nullptr), fontInfo);
            DWORD fontsAdded = 0;
            if (fontData) app.fontResource = AddFontMemResourceEx(LockResource(fontData), SizeofResource(GetModuleHandleW(nullptr), fontInfo), nullptr, &fontsAdded);
        }
        const wchar_t* face = app.fontResource ? L"Manrope" : L"Segoe UI";
        app.regular = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        app.medium = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        app.small = CreateFontW(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        app.title = CreateFontW(-25, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        app.model.load();
        std::wstring autostartError;
        app.autostartEnabled = isAutostartEnabled(autostartError);
        app.autostart = button(L"", ID_AUTOSTART); updateAutostartButton();
        app.banner = control(L"STATIC", L"", SS_OWNERDRAW, 0); ShowWindow(app.banner, SW_HIDE);
        app.status = control(L"STATIC", L"Добавьте подписку и выберите сервер", SS_LEFT | SS_CENTERIMAGE, 0); setFont(app.status, app.medium);
        app.connect = button(L"Подключить", ID_CONNECT); setFont(app.connect, app.medium);
        app.groups = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, ID_GROUPS);
        app.profiles = control(L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL, ID_PROFILES);
        app.url = control(L"EDIT", L"", ES_AUTOHSCROLL, ID_URL);
        SendMessageW(app.url, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
        SendMessageW(app.url, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"https://… подписка или vless://… сервер"));
        app.add = button(L"Добавить", ID_ADD_SUBSCRIPTION);
        app.update = button(L"Обновить", ID_UPDATE_GROUP);
        app.remove = button(L"Удалить группу", ID_DELETE_GROUP);
        app.removeProfile = button(L"Удалить сервер", ID_DELETE_PROFILE);
        setFont(app.update, app.small); setFont(app.remove, app.small); setFont(app.removeProfile, app.small);
        app.address = control(L"EDIT", app.model.listenAddress.c_str(), ES_AUTOHSCROLL, ID_ADDRESS);
        app.port = control(L"EDIT", std::to_wstring(app.model.listenPort).c_str(), ES_NUMBER, ID_PORT);
        SendMessageW(app.address, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
        SendMessageW(app.port, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
        app.processRouting = control(L"BUTTON", L"Направлять выбранные процессы через VPN", BS_AUTOCHECKBOX | WS_TABSTOP, ID_PROCESS_ROUTING);
        setFont(app.processRouting, app.small);
        SendMessageW(app.processRouting, BM_SETCHECK, app.model.processRoutingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        app.refreshProcesses = button(L"Обновить", ID_REFRESH_PROCESSES);
        app.filterStatus = control(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 0); setFont(app.filterStatus, app.small);
        app.runningProcesses = control(L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL, ID_RUNNING_PROCESSES);
        app.selectedProcesses = control(L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL, ID_SELECTED_PROCESSES);
        app.addProcess = button(L"Добавить →", ID_ADD_PROCESS);
        app.removeProcess = button(L"← Убрать", ID_REMOVE_PROCESS);
        app.log = control(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, ID_LOG);
        button(L"Копировать", ID_COPY_LOG);
        button(L"Очистить", ID_CLEAR_LOG);
        refillGroups(); refillSelectedProcesses(); refreshRunningProcesses(); updateFilterStatus(); appendLog(L"Нативное приложение запущено");
        if (!autostartError.empty()) appendLog(L"Автозагрузка: " + autostartError);
        SetTimer(hwnd, FILTER_STATUS_TIMER, 1500, nullptr);
        showBanner(L"Hysteria2 и VLESS XHTTP готовы; выберите сервер и подключитесь");
        return 0;
    }
    case WM_SIZE: layout(LOWORD(lParam), HIWORD(lParam)); return 0;
    case WM_TIMER:
        if (wParam == FILTER_STATUS_TIMER) updateFilterStatus(true);
        return 0;
    case WM_TRAY: {
        UINT event = LOWORD(lParam);
        if (event == WM_LBUTTONDBLCLK || event == NIN_SELECT || event == NIN_KEYSELECT) {
            restoreWindow();
        } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            POINT point{};
            if (event == WM_CONTEXTMENU) {
                point.x = GET_X_LPARAM(wParam);
                point.y = GET_Y_LPARAM(wParam);
            } else {
                GetCursorPos(&point);
            }
            showTrayMenu(point);
        }
        return 0;
    }
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
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure->CtlID == ID_PROFILES) measure->itemHeight = 68;
        else if (measure->CtlID == ID_RUNNING_PROCESSES || measure->CtlID == ID_SELECTED_PROCESSES) measure->itemHeight = 30;
        else if (measure->CtlID == ID_GROUPS) measure->itemHeight = 36;
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item.hwndItem == app.banner) drawBanner(item);
        else if (item.CtlType == ODT_LISTBOX || item.CtlType == ODT_COMBOBOX) drawChoice(item);
        else drawButton(item);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam), notification = HIWORD(wParam);
        if (id == ID_TRAY_OPEN) {
            restoreWindow();
        } else if (id == ID_TRAY_TOGGLE) {
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(ID_CONNECT, BN_CLICKED), 0);
        } else if (id == ID_TRAY_EXIT) {
            app.exiting = true;
            saveListenerFields();
            DestroyWindow(hwnd);
        } else if (id == ID_AUTOSTART) {
            std::wstring error;
            bool requested = !app.autostartEnabled;
            if (setAutostartEnabled(requested, error)) {
                app.autostartEnabled = requested; updateAutostartButton();
                std::wstring state = requested ? L"Автозагрузка включена" : L"Автозагрузка выключена";
                showBanner(state); appendLog(state);
            } else {
                showBanner(error.empty() ? L"Не удалось изменить автозагрузку" : error, true);
                appendLog(L"Ошибка автозагрузки: " + error);
            }
        } else if (id == ID_ADD_SUBSCRIPTION) addSubscriptionOrProfile(windowText(app.url));
        else if (id == ID_PROCESS_ROUTING && notification == BN_CLICKED) {
            if (app.connecting || app.disconnecting) {
                SendMessageW(app.processRouting, BM_SETCHECK, app.model.processRoutingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                showBanner(L"Дождитесь завершения подключения или отключения");
                return 0;
            }
            app.model.processRoutingEnabled = SendMessageW(app.processRouting, BM_GETCHECK, 0, 0) == BST_CHECKED;
            app.model.save();
            if (app.session) beginDisconnect(app.activeProfileId);
            updateFilterStatus();
            showBanner(app.model.processRoutingEnabled ? L"VPN для выбранных процессов включён" : L"VPN для процессов выключен");
        }
        else if (id == ID_DELETE_PROFILE) {
            if (app.connecting || app.disconnecting || app.busy) {
                showBanner(L"Дождитесь завершения текущей операции"); return 0;
            }
            int row = static_cast<int>(SendMessageW(app.profiles, LB_GETCURSEL, 0, 0));
            if (row == LB_ERR) { showBanner(L"Выберите сервер для удаления", true); return 0; }
            size_t index = static_cast<size_t>(SendMessageW(app.profiles, LB_GETITEMDATA, row, 0));
            if (index >= app.model.profiles.size()) return 0;
            const auto profile = app.model.profiles[index];
            if (app.session && app.activeProfileId == profile.id) beginDisconnect();
            app.model.deleteProfile(profile.id);
            refillProfiles(); app.model.save();
            appendLog(L"Удалён сервер: " + profile.name);
            showBanner(profile.groupId.empty() ? L"Сервер удалён" : L"Сервер удалён. Обновление подписки может вернуть его");
        }
        else if (id == ID_REFRESH_PROCESSES) refreshRunningProcesses();
        else if (id == ID_ADD_PROCESS || (id == ID_RUNNING_PROCESSES && notification == LBN_DBLCLK)) addSelectedProcess();
        else if (id == ID_REMOVE_PROCESS || (id == ID_SELECTED_PROCESSES && notification == LBN_DBLCLK)) removeSelectedProcess();
        else if (id == ID_UPDATE_GROUP) {
            int selected = static_cast<int>(SendMessageW(app.groups, CB_GETCURSEL, 0, 0));
            if (selected <= 0 || static_cast<size_t>(selected - 1) >= app.model.groups.size()) showBanner(L"Выберите конкретную группу для обновления", true);
            else { const auto& group = app.model.groups[static_cast<size_t>(selected - 1)]; beginDownload(group.url, group.id); }
        } else if (id == ID_DELETE_GROUP) {
            if (app.connecting || app.disconnecting || app.busy) {
                showBanner(L"Дождитесь завершения текущей операции"); return 0;
            }
            int selected = static_cast<int>(SendMessageW(app.groups, CB_GETCURSEL, 0, 0));
            if (selected <= 0 || static_cast<size_t>(selected - 1) >= app.model.groups.size()) showBanner(L"Выберите группу для удаления", true);
            else {
                const auto group = app.model.groups[static_cast<size_t>(selected - 1)];
                const bool removesActive = std::any_of(app.model.profiles.begin(), app.model.profiles.end(),
                    [&](const auto& profile) { return profile.groupId == group.id && profile.id == app.activeProfileId; });
                if (app.session && removesActive) beginDisconnect();
                app.model.deleteGroup(group.id);
                refillGroups(); app.model.save();
                appendLog(L"Удалена группа: " + group.name); showBanner(L"Группа удалена");
            }
        } else if (id == ID_PROFILES && notification == LBN_SELCHANGE) {
            int selected = static_cast<int>(SendMessageW(app.profiles, LB_GETCURSEL, 0, 0));
            if (selected >= 0) {
                size_t index = static_cast<size_t>(SendMessageW(app.profiles, LB_GETITEMDATA, selected, 0));
                app.model.selectedProfileId = app.model.profiles[index].id; app.model.save(); SetWindowTextW(app.status, app.model.profiles[index].name.c_str());
                if (app.session) {
                    appendLog(L"Останавливаю старый туннель при смене сервера");
                    beginDisconnect(app.model.profiles[index].id);
                }
            }
        } else if (id == ID_GROUPS && notification == CBN_SELCHANGE) {
            refillProfiles();
        } else if (id == ID_CONNECT) {
            saveListenerFields();
            if (app.session) {
                beginDisconnect();
                return 0;
            }
            auto selected = std::find_if(app.model.profiles.begin(), app.model.profiles.end(), [&](const auto& item) { return item.id == app.model.selectedProfileId; });
            if (selected == app.model.profiles.end()) {
                showBanner(L"Сначала выберите сервер", true);
            } else {
                beginConnect(selected->id);
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
            HWND target = app.window;
            app.session->setErrorHandler([target](std::wstring error) {
                auto message = std::make_unique<std::wstring>(std::move(error));
                if (!PostMessageW(target, WM_TUNNEL_ERROR, 0, reinterpret_cast<LPARAM>(message.get()))) return;
                message.release();
            });
            std::wstring listenerError;
            app.socks = SocksServer::start(app.model.listenAddress, app.model.listenPort, *app.session, listenerError);
            if (!app.socks) {
                app.session.reset();
                SetWindowTextW(app.connect, L"Подключить");
                showBanner(listenerError, true); appendLog(L"Ошибка SOCKS5: " + listenerError);
                return 0;
            }
            if (app.model.processRoutingEnabled && !app.model.filteredProcesses.empty()) {
                app.lastMatched = app.lastRedirected = app.lastProxyReplies = app.lastAccepted = app.lastAcceptAttempts = app.lastNatMisses =
                    app.lastUdpMatched = app.lastUdpSent = app.lastUdpReceived = app.lastInjectionFailures = 0;
                app.processFilter = ProcessFilter::start(app.model.filteredProcesses, *app.session, listenerError);
                if (!app.processFilter) {
                    app.socks.reset(); app.session.reset();
                    SetWindowTextW(app.connect, L"Подключить");
                    showBanner(listenerError, true); appendLog(L"Ошибка фильтра процессов: " + listenerError);
                    return 0;
                }
            }
            updateFilterStatus();
            SetWindowTextW(app.connect, L"Отключить");
            updateTrayIcon();
            std::wstring ready = app.session->supportsUdp()
                ? L"SOCKS5 TCP+UDP + HTTP CONNECT работают на "
                : L"SOCKS5 TCP + HTTP CONNECT работают на ";
            ready += app.model.listenAddress + L":" +
                std::to_wstring(app.model.listenPort);
            if (app.processFilter) ready += app.session->supportsUdp()
                ? L"; фильтр TCP+UDP (IPv4/IPv6): " + std::to_wstring(app.model.filteredProcesses.size()) + L" процесс(ов) — перезапустите их"
                : L"; фильтр TCP (IPv4/IPv6): " + std::to_wstring(app.model.filteredProcesses.size()) + L" процесс(ов) — VLESS UDP пока не включён";
            showBanner(ready); appendLog(payload->result.message); appendLog(ready);
        } else {
            SetWindowTextW(app.connect, L"Подключить");
            updateTrayIcon();
            showBanner(payload->result.message.empty() ? L"VPN не подключился" : payload->result.message, true);
            appendLog(L"Ошибка подключения: " + payload->result.message);
        }
        return 0;
    }
    case WM_DISCONNECT_READY: {
        std::unique_ptr<DisconnectPayload> payload(reinterpret_cast<DisconnectPayload*>(lParam));
        app.disconnecting = false;
        updateTrayIcon();
        updateFilterStatus();
        SetWindowTextW(app.connect, L"Подключить");
        EnableWindow(app.connect, TRUE);
        EnableWindow(app.profiles, TRUE);
        showBanner(L"VPN отключён");
        appendLog(L"Соединение остановлено");
        if (payload && !payload->reconnectProfileId.empty()) {
            appendLog(L"Подключаю выбранный сервер");
            beginConnect(payload->reconnectProfileId);
        }
        return 0;
    }
    case WM_TUNNEL_ERROR: {
        std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lParam));
        if (app.session && error && !error->empty()) {
            if (error->rfind(L"TCP-запрос не установился", 0) == 0 ||
                error->rfind(L"UDP-запрос не отправлен", 0) == 0) {
                appendLog(*error);
            } else {
                showBanner(*error, true);
                appendLog(L"Ошибка туннеля: " + *error);
            }
        }
        return 0;
    }
    case WM_CLOSE:
        saveListenerFields();
        if (!app.exiting && app.trayAdded) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, FILTER_STATUS_TIMER);
        removeTrayIcon();
        if (app.session) app.session->stop();
        app.processFilter.reset();
        app.socks.reset();
        app.session.reset();
        DeleteObject(app.regular); DeleteObject(app.medium); DeleteObject(app.small); DeleteObject(app.title); DeleteObject(app.background); DeleteObject(app.card); DeleteObject(app.input);
        if (app.fontResource) RemoveFontMemResourceEx(app.fontResource);
        PostQuitMessage(0); return 0;
    }
    if (taskbarCreatedMessage && message == taskbarCreatedMessage) {
        app.trayAdded = false;
        updateTrayIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW; cls.lpfnWndProc = windowProc; cls.hInstance = instance;
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); cls.hIconSm = cls.hIcon;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = nullptr; cls.lpszClassName = L"BigHeadVPNNativeWindow";
    if (!RegisterClassExW(&cls)) return 1;
    taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    BOOL dark = TRUE;
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"Big Head VPN", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, nullptr, nullptr, instance, nullptr);
    if (!window) return 2;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    bool startedAutomatically = commandLine && wcsstr(commandLine, L"--autostart");
    ShowWindow(window, startedAutomatically ? SW_HIDE : show); UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return static_cast<int>(message.wParam);
}
