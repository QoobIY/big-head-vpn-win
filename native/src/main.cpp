#include "model.h"
#include "hysteria_client.h"
#include "subscription.h"
#include "socks_server.h"
#include "process_filter.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>

namespace {
constexpr COLORREF BACKGROUND = RGB(18, 20, 25);
constexpr COLORREF SIDEBAR = RGB(12, 14, 18);
constexpr COLORREF CARD = RGB(25, 28, 35);
constexpr COLORREF INPUT = RGB(15, 17, 22);
constexpr COLORREF BORDER = RGB(43, 47, 58);
constexpr COLORREF TEXT = RGB(246, 247, 251);
constexpr COLORREF MUTED = RGB(157, 164, 178);
constexpr COLORREF ACCENT = RGB(88, 101, 242);
constexpr COLORREF ERROR_RED = RGB(218, 55, 60);

enum ControlId {
    ID_NAV_HOME = 100, ID_NAV_SERVERS, ID_NAV_SETTINGS,
    ID_CONNECT, ID_GROUPS, ID_PROFILES, ID_URL, ID_ADD_SUBSCRIPTION,
    ID_UPDATE_GROUP, ID_DELETE_GROUP, ID_LOG, ID_COPY_LOG, ID_CLEAR_LOG,
    ID_ADDRESS, ID_PORT, ID_RUNNING_PROCESSES, ID_SELECTED_PROCESSES,
    ID_REFRESH_PROCESSES, ID_ADD_PROCESS, ID_REMOVE_PROCESS
};
constexpr UINT WM_SUBSCRIPTION_READY = WM_APP + 10;
constexpr UINT WM_CONNECT_READY = WM_APP + 11;
constexpr UINT_PTR FILTER_STATUS_TIMER = 1;
constexpr int IDR_MANROPE = 101;

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
    HWND runningProcesses{}, selectedProcesses{}, refreshProcesses{}, addProcess{}, removeProcess{};
    HWND filterStatus{};
    HFONT regular{}, medium{}, title{}, small{};
    HBRUSH background{}, sidebar{}, card{}, input{};
    HANDLE fontResource{};
    AppModel model;
    bool busy{};
    bool connecting{};
    std::unique_ptr<HysteriaClient> session;
    std::unique_ptr<SocksServer> socks;
    std::unique_ptr<ProcessFilter> processFilter;
    unsigned long long lastMatched{}, lastRedirected{}, lastProxyReplies{}, lastAccepted{}, lastAcceptAttempts{}, lastNatMisses{}, lastUdpMatched{}, lastUdpSent{}, lastUdpReceived{}, lastInjectionFailures{};
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
    if (app.session) showBanner(message + L". Переподключите VPN, чтобы применить список");
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
        SetWindowTextW(app.filterStatus, app.model.filteredProcesses.empty()
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
    auto injectionFailures = app.processFilter->injectionFailures();
    std::wstring status = L"TCP: соединений " + std::to_wstring(matched) +
        L"  •  SYN " + std::to_wstring(redirected) + L"/SYN-ACK " + std::to_wstring(proxyReplies) + L"  •  relay " + std::to_wstring(accepted);
    if (attempts != accepted) status += L"/" + std::to_wstring(attempts) + L" (NAT miss " + std::to_wstring(natMisses) + L")";
    if (injectionFailures) status += L"  •  send errors " + std::to_wstring(injectionFailures) + L"/" + std::to_wstring(app.processFilter->lastInjectionError());
    status += L"  •  UDP " + std::to_wstring(udpMatched) + L":" + std::to_wstring(udpSent) + L"/" + std::to_wstring(udpReceived);
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
    int side = 190, margin = 24, top = 126;
    MoveWindow(app.banner, side + margin, 68, width - side - margin * 2, 40, TRUE);
    HWND home = GetDlgItem(app.window, ID_NAV_HOME), servers = GetDlgItem(app.window, ID_NAV_SERVERS), settings = GetDlgItem(app.window, ID_NAV_SETTINGS);
    MoveWindow(home, 16, 96, side - 32, 44, TRUE); MoveWindow(servers, 16, 148, side - 32, 44, TRUE); MoveWindow(settings, 16, 200, side - 32, 44, TRUE);
    int contentX = side + margin, contentW = width - contentX - margin;
    int leftW = std::max(360, contentW * 55 / 100), gap = 18, rightX = contentX + leftW + gap, rightW = contentW - leftW - gap;
    MoveWindow(app.status, contentX + 20, top + 18, leftW - 40, 28, TRUE);
    MoveWindow(app.connect, contentX + 20, top + 56, leftW - 40, 44, TRUE);
    MoveWindow(app.groups, contentX + 20, top + 140, leftW - 40, 240, TRUE);
    int listH = std::max(120, height - top - 376);
    MoveWindow(app.profiles, contentX + 20, top + 190, leftW - 40, listH, TRUE);
    MoveWindow(app.url, contentX + 20, top + 202 + listH, leftW - 158, 38, TRUE);
    MoveWindow(app.add, contentX + leftW - 128, top + 202 + listH, 108, 38, TRUE);
    MoveWindow(app.update, contentX + 20, top + 252 + listH, 122, 36, TRUE);
    MoveWindow(app.remove, contentX + 152, top + 252 + listH, 136, 36, TRUE);
    MoveWindow(app.address, rightX + 20, top + 30, std::max(100, rightW - 146), 38, TRUE);
    MoveWindow(app.port, rightX + rightW - 114, top + 30, 94, 38, TRUE);
    int processListTop = top + 150;
    int processListHeight = std::clamp((height - top) * 28 / 100, 110, 180);
    int processWidth = (rightW - 50) / 2;
    MoveWindow(app.refreshProcesses, rightX + rightW - 116, top + 82, 96, 32, TRUE);
    MoveWindow(app.filterStatus, rightX + 20, top + 102, rightW - 40, 24, TRUE);
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
    RECT side{0, 0, 190, client.bottom}; FillRect(dc, &side, app.sidebar);
    RECT left{214, 126, 214 + std::max(360L, (client.right - 238) * 55L / 100), client.bottom - 18}; roundedBox(dc, left, CARD, BORDER);
    int rightX = left.right + 18;
    RECT right{rightX, 126, client.right - 24, client.bottom - 18}; roundedBox(dc, right, CARD, BORDER);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, TEXT);
    SelectObject(dc, app.title);
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 38, 38, LR_DEFAULTCOLOR));
    if (icon) { DrawIconEx(dc, 18, 20, icon, 38, 38, 0, nullptr, DI_NORMAL); DestroyIcon(icon); }
    TextOutW(dc, 68, 27, L"BIG HEAD", 8);
    TextOutW(dc, 214, 24, L"Big Head VPN", 12);
    SelectObject(dc, app.regular); SetTextColor(dc, MUTED);
    constexpr wchar_t subtitle[] = L"Лёгкий нативный клиент для Windows";
    TextOutW(dc, 215, 49, subtitle, static_cast<int>(std::size(subtitle) - 1));
    label(dc, 234, 247, L"СЕРВЕРЫ И ПОДПИСКИ");
    label(dc, rightX + 20, 143, L"ЛОКАЛЬНЫЙ SOCKS5");
    label(dc, rightX + 20, 213, L"ПРОЦЕССЫ ЧЕРЕЗ VPN");
    label(dc, rightX + 20, 258, L"ЗАПУЩЕННЫЕ");
    int processWidth = (right.right - right.left - 50) / 2;
    label(dc, rightX + 30 + processWidth, 258, L"В VPN");
    int processListHeight = std::clamp(static_cast<int>((client.bottom - 126) * 28 / 100), 110, 180);
    label(dc, rightX + 20, 126 + 150 + processListHeight + 55, L"ЖУРНАЛ СОЕДИНЕНИЯ");
}

void drawButton(const DRAWITEMSTRUCT& item) {
    bool primary = item.CtlID == ID_CONNECT || item.CtlID == ID_ADD_SUBSCRIPTION || item.CtlID == ID_ADD_PROCESS;
    bool disabled = (item.itemState & ODS_DISABLED) != 0;
    bool home = item.CtlID == ID_NAV_HOME;
    COLORREF color = disabled ? RGB(54, 57, 66) : primary ? ACCENT : home ? RGB(45, 50, 69) : RGB(47, 51, 61);
    if ((item.itemState & ODS_SELECTED) && !disabled) color = primary ? RGB(71, 82, 196) : RGB(62, 64, 70);
    bool navigation = item.CtlID == ID_NAV_HOME || item.CtlID == ID_NAV_SERVERS || item.CtlID == ID_NAV_SETTINGS;
    RECT background = item.rcItem; FillRect(item.hDC, &background, navigation ? app.sidebar : app.card);
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
    bool selected = (item.itemState & ODS_SELECTED) != 0;
    COLORREF fill = selected ? RGB(45, 50, 69) : INPUT;
    FillRect(item.hDC, &item.rcItem, app.input);
    RECT row = item.rcItem; InflateRect(&row, -2, -2);
    if (selected) roundedBox(item.hDC, row, fill, fill, 8);
    wchar_t text[1024]{};
    if (item.CtlType == ODT_LISTBOX) SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(text));
    else SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text));
    SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, selected ? TEXT : RGB(211, 215, 224));
    SelectObject(item.hDC, selected ? app.medium : app.regular);
    RECT textRect = item.rcItem; textRect.left += 12; textRect.right -= 8;
    DrawTextW(item.hDC, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        app.window = hwnd;
        app.background = CreateSolidBrush(BACKGROUND); app.sidebar = CreateSolidBrush(SIDEBAR); app.card = CreateSolidBrush(CARD); app.input = CreateSolidBrush(INPUT);
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
        button(L"Главная", ID_NAV_HOME);
        button(L"Серверы", ID_NAV_SERVERS);
        button(L"Настройки", ID_NAV_SETTINGS);
        app.banner = control(L"STATIC", L"", SS_OWNERDRAW, 0); ShowWindow(app.banner, SW_HIDE);
        app.status = control(L"STATIC", L"Добавьте подписку и выберите сервер", SS_LEFT | SS_CENTERIMAGE, 0); setFont(app.status, app.medium);
        app.connect = button(L"Подключить", ID_CONNECT); setFont(app.connect, app.medium);
        app.groups = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, ID_GROUPS);
        app.profiles = control(L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL, ID_PROFILES);
        app.url = control(L"EDIT", L"", ES_AUTOHSCROLL, ID_URL);
        SendMessageW(app.url, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
        SendMessageW(app.url, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"https://… ссылка подписки"));
        app.add = button(L"Добавить", ID_ADD_SUBSCRIPTION);
        app.update = button(L"Обновить", ID_UPDATE_GROUP);
        app.remove = button(L"Удалить группу", ID_DELETE_GROUP);
        app.address = control(L"EDIT", app.model.listenAddress.c_str(), ES_AUTOHSCROLL, ID_ADDRESS);
        app.port = control(L"EDIT", std::to_wstring(app.model.listenPort).c_str(), ES_NUMBER, ID_PORT);
        SendMessageW(app.address, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
        SendMessageW(app.port, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
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
        SetTimer(hwnd, FILTER_STATUS_TIMER, 1500, nullptr);
        showBanner(L"Hysteria2 TCP через SOCKS5 готов; выберите сервер и подключитесь");
        return 0;
    }
    case WM_SIZE: layout(LOWORD(lParam), HIWORD(lParam)); return 0;
    case WM_TIMER:
        if (wParam == FILTER_STATUS_TIMER) updateFilterStatus(true);
        return 0;
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
        if (measure->CtlID == ID_PROFILES) measure->itemHeight = 32;
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
        if (id == ID_ADD_SUBSCRIPTION) beginDownload(windowText(app.url), L"");
        else if (id == ID_REFRESH_PROCESSES) refreshRunningProcesses();
        else if (id == ID_ADD_PROCESS || (id == ID_RUNNING_PROCESSES && notification == LBN_DBLCLK)) addSelectedProcess();
        else if (id == ID_REMOVE_PROCESS || (id == ID_SELECTED_PROCESSES && notification == LBN_DBLCLK)) removeSelectedProcess();
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
                    app.processFilter.reset(); app.socks.reset(); app.session.reset(); SetWindowTextW(app.connect, L"Подключить");
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
                app.processFilter.reset();
                app.socks.reset();
                app.session.reset();
                updateFilterStatus();
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
            if (!app.model.filteredProcesses.empty()) {
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
            std::wstring ready = L"SOCKS5 работает на " + app.model.listenAddress + L":" + std::to_wstring(app.model.listenPort);
            if (app.processFilter) ready += L"; фильтр TCP+UDP (IPv4/IPv6): " + std::to_wstring(app.model.filteredProcesses.size()) + L" процесс(ов) — перезапустите их";
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
        KillTimer(hwnd, FILTER_STATUS_TIMER);
        app.processFilter.reset();
        app.socks.reset();
        app.session.reset();
        DeleteObject(app.regular); DeleteObject(app.medium); DeleteObject(app.small); DeleteObject(app.title); DeleteObject(app.background); DeleteObject(app.sidebar); DeleteObject(app.card); DeleteObject(app.input);
        if (app.fontResource) RemoveFontMemResourceEx(app.fontResource);
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
