#include "autostart.h"

#include <windows.h>
#include <taskschd.h>
#include <oleauto.h>
#include <shlobj.h>

#include <utility>

namespace {
constexpr wchar_t TASK_NAME[] = L"Big Head VPN";

template<class T>
class ComObject {
public:
    ~ComObject() { if (value_) value_->Release(); }
    T** put() { return &value_; }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
private:
    T* value_{};
};

class BString {
public:
    explicit BString(const wchar_t* value) : value_(SysAllocString(value)) {}
    explicit BString(const std::wstring& value) : value_(SysAllocStringLen(value.data(), static_cast<UINT>(value.size()))) {}
    ~BString() { SysFreeString(value_); }
    operator BSTR() const { return value_; }
private:
    BSTR value_{};
};

class ComSession {
public:
    ComSession() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComSession() { if (SUCCEEDED(result_)) CoUninitialize(); }
    bool ready() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT result_;
};

std::wstring systemMessage(HRESULT result) {
    wchar_t* raw{};
    DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring message = size && raw ? std::wstring(raw, size) : L"код 0x" + std::to_wstring(static_cast<unsigned long>(result));
    if (raw) LocalFree(raw);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) message.pop_back();
    return message;
}

bool openScheduler(ComObject<ITaskService>& service, ComObject<ITaskFolder>& root, std::wstring& error) {
    HRESULT result = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
        reinterpret_cast<void**>(service.put()));
    if (FAILED(result)) { error = L"Не удалось открыть Планировщик заданий: " + systemMessage(result); return false; }
    VARIANT empty; VariantInit(&empty);
    result = service->Connect(empty, empty, empty, empty);
    if (FAILED(result)) { error = L"Не удалось подключиться к Планировщику заданий: " + systemMessage(result); return false; }
    BString rootPath(L"\\");
    result = service->GetFolder(rootPath, root.put());
    if (FAILED(result)) { error = L"Не удалось открыть библиотеку заданий: " + systemMessage(result); return false; }
    return true;
}

std::wstring executablePath() {
    std::wstring result(32768, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
    if (!size || size >= result.size()) return {};
    result.resize(size);
    return result;
}

std::wstring startupShortcut(std::wstring& error) {
    PWSTR folder{};
    HRESULT result = SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_CREATE, nullptr, &folder);
    if (FAILED(result)) { error = L"Не удалось открыть папку автозагрузки: " + systemMessage(result); return {}; }
    std::wstring path = std::wstring(folder) + L"\\Big Head VPN.lnk";
    CoTaskMemFree(folder);
    return path;
}

bool createStartupShortcut(const std::wstring& shortcut, const std::wstring& executable, std::wstring& error) {
    // Explorer starts an unelevated GUI launcher without a console window.
    // The demand-only task supplies the rights required by WinDivert.
    std::wstring launcher = executable.substr(0, executable.find_last_of(L"\\/") + 1) + L"BigHeadVPNStartup.exe";
    if (GetFileAttributesW(launcher.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"Не найден BigHeadVPNStartup.exe рядом с приложением"; return false;
    }
    ComObject<IShellLinkW> link;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
        reinterpret_cast<void**>(link.put()));
    if (SUCCEEDED(result)) result = link->SetPath(launcher.c_str());
    if (SUCCEEDED(result)) result = link->SetArguments(L"");
    if (SUCCEEDED(result)) result = link->SetDescription(L"Запуск Big Head VPN при входе в Windows");
    if (SUCCEEDED(result)) result = link->SetIconLocation(executable.c_str(), 0);
    if (SUCCEEDED(result)) result = link->SetShowCmd(SW_SHOWNORMAL);
    ComObject<IPersistFile> file;
    if (SUCCEEDED(result)) result = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.put()));
    if (SUCCEEDED(result)) result = file->Save(shortcut.c_str(), TRUE);
    if (FAILED(result)) { error = L"Не удалось создать ярлык автозагрузки: " + systemMessage(result); return false; }
    return true;
}

bool startupDisabledByWindows() {
    // Windows stores Task Manager's per-entry switch separately from the link.
    // Treat unknown/missing formats as enabled; never rewrite the binary format.
    BYTE state[12]{};
    DWORD size = sizeof(state);
    DWORD type{};
    LSTATUS result = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder",
        L"Big Head VPN.lnk", RRF_RT_REG_BINARY, &type, state, &size);
    return result == ERROR_SUCCESS && size == sizeof(state) && (state[0] == 3 || state[0] == 7);
}

}

bool isAutostartEnabled(std::wstring& error) {
    error.clear();
    ComSession session;
    if (!session.ready()) { error = L"Не удалось инициализировать Windows COM"; return false; }
    ComObject<ITaskService> service; ComObject<ITaskFolder> root;
    if (!openScheduler(service, root, error)) return false;
    ComObject<IRegisteredTask> task;
    BString name(TASK_NAME);
    HRESULT result = root->GetTask(name, task.put());
    if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return false;
    if (FAILED(result)) { error = L"Не удалось проверить автозагрузку: " + systemMessage(result); return false; }
    VARIANT_BOOL enabled = VARIANT_FALSE;
    result = task->get_Enabled(&enabled);
    if (FAILED(result)) { error = L"Не удалось прочитать состояние автозагрузки: " + systemMessage(result); return false; }
    if (enabled != VARIANT_TRUE) return false;
    auto shortcut = startupShortcut(error);
    if (shortcut.empty()) return false;
    if (GetFileAttributesW(shortcut.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Upgrade the old logon-trigger task to visible Explorer startup once.
        ComObject<ITaskDefinition> definition;
        ComObject<ITriggerCollection> triggers;
        LONG count{};
        if (SUCCEEDED(task->get_Definition(definition.put())) &&
            SUCCEEDED(definition->get_Triggers(triggers.put())) &&
            SUCCEEDED(triggers->get_Count(&count)) && count > 0)
            return setAutostartEnabled(true, error);
        return false;
    }
    return !startupDisabledByWindows();
}

bool setAutostartEnabled(bool enabled, std::wstring& error) {
    error.clear();
    ComSession session;
    if (!session.ready()) { error = L"Не удалось инициализировать Windows COM"; return false; }
    ComObject<ITaskService> service; ComObject<ITaskFolder> root;
    if (!openScheduler(service, root, error)) return false;
    BString name(TASK_NAME);
    auto shortcut = startupShortcut(error);
    if (shortcut.empty()) return false;
    if (!enabled) {
        if (!DeleteFileW(shortcut.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            error = L"Не удалось удалить ярлык автозагрузки: " + systemMessage(HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }
        HRESULT result = root->DeleteTask(name, 0);
        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return true;
        if (FAILED(result)) { error = L"Не удалось отключить автозагрузку: " + systemMessage(result); return false; }
        return true;
    }

    auto path = executablePath();
    if (path.empty()) { error = L"Не удалось определить путь к BigHeadVPN.exe"; return false; }
    ComObject<ITaskDefinition> task;
    HRESULT result = service->NewTask(0, task.put());
    if (FAILED(result)) { error = L"Не удалось создать задание автозагрузки: " + systemMessage(result); return false; }

    ComObject<IRegistrationInfo> registration;
    if (SUCCEEDED(task->get_RegistrationInfo(registration.put()))) {
        BString description(L"Запуск Big Head VPN при входе пользователя в Windows");
        registration->put_Description(description);
    }
    ComObject<IPrincipal> principal;
    if (FAILED(task->get_Principal(principal.put())) ||
        FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
        FAILED(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST))) {
        error = L"Не удалось настроить права задания автозагрузки"; return false;
    }
    ComObject<ITaskSettings> settings;
    BString unlimited(L"PT0S");
    if (FAILED(task->get_Settings(settings.put())) ||
        FAILED(settings->put_Enabled(VARIANT_TRUE)) ||
        FAILED(settings->put_AllowDemandStart(VARIANT_TRUE)) ||
        FAILED(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW)) ||
        FAILED(settings->put_ExecutionTimeLimit(unlimited)) ||
        FAILED(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
        FAILED(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE))) {
        error = L"Не удалось настроить задание автозагрузки"; return false;
    }
    ComObject<IActionCollection> actions;
    ComObject<IAction> action;
    if (FAILED(task->get_Actions(actions.put())) || FAILED(actions->Create(TASK_ACTION_EXEC, action.put()))) {
        error = L"Не удалось создать действие автозагрузки"; return false;
    }
    ComObject<IExecAction> execute;
    result = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(execute.put()));
    if (FAILED(result)) { error = L"Не удалось настроить запуск приложения"; return false; }
    BString executable(path), arguments(L"--autostart");
    BString directory(path.substr(0, path.find_last_of(L"\\/")));
    if (FAILED(execute->put_Path(executable)) || FAILED(execute->put_Arguments(arguments)) || FAILED(execute->put_WorkingDirectory(directory))) {
        error = L"Не удалось записать путь приложения в автозагрузку"; return false;
    }
    VARIANT empty; VariantInit(&empty);
    ComObject<IRegisteredTask> registered;
    result = root->RegisterTaskDefinition(name, task.get(), TASK_CREATE_OR_UPDATE, empty, empty,
        TASK_LOGON_INTERACTIVE_TOKEN, empty, registered.put());
    if (FAILED(result)) { error = L"Не удалось включить автозагрузку: " + systemMessage(result); return false; }
    if (!createStartupShortcut(shortcut, path, error)) return false;
    HKEY approval{};
    LSTATUS approvalResult = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder",
        0, KEY_SET_VALUE, &approval);
    if (approvalResult == ERROR_SUCCESS) {
        approvalResult = RegDeleteValueW(approval, L"Big Head VPN.lnk");
        RegCloseKey(approval);
    }
    if (approvalResult != ERROR_SUCCESS && approvalResult != ERROR_FILE_NOT_FOUND) {
        error = L"Не удалось включить автозагрузку в Windows: " + systemMessage(HRESULT_FROM_WIN32(approvalResult));
        return false;
    }
    return true;
}

#ifdef BIG_HEAD_VPN_TESTING
bool createStartupShortcutForTest(const std::wstring& shortcut, std::wstring& error) {
    ComSession session;
    if (!session.ready()) { error = L"COM initialization failed"; return false; }
    return createStartupShortcut(shortcut, executablePath(), error);
}
#endif

bool runAutostartTask(std::wstring& error) {
    ComSession session;
    if (!session.ready()) { error = L"Не удалось инициализировать Windows COM"; return false; }
    ComObject<ITaskService> service;
    ComObject<ITaskFolder> root;
    if (!openScheduler(service, root, error)) return false;
    BString name(TASK_NAME);
    ComObject<IRegisteredTask> task;
    HRESULT result = root->GetTask(name, task.put());
    VARIANT empty; VariantInit(&empty);
    ComObject<IRunningTask> running;
    if (SUCCEEDED(result)) result = task->Run(empty, running.put());
    if (FAILED(result)) { error = systemMessage(result); return false; }
    return true;
}

bool removeOwnedAutostart(std::wstring& error) {
    ComSession session;
    if (!session.ready()) { error = L"Не удалось инициализировать Windows COM"; return false; }
    ComObject<ITaskService> service;
    ComObject<ITaskFolder> root;
    if (!openScheduler(service, root, error)) return false;
    BString name(TASK_NAME);
    ComObject<IRegisteredTask> task;
    HRESULT result = root->GetTask(name, task.put());
    if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return true;
    ComObject<ITaskDefinition> definition;
    ComObject<IActionCollection> actions;
    ComObject<IAction> action;
    ComObject<IExecAction> execute;
    if (SUCCEEDED(result)) result = task->get_Definition(definition.put());
    if (SUCCEEDED(result)) result = definition->get_Actions(actions.put());
    if (SUCCEEDED(result)) result = actions->get_Item(1, action.put());
    if (SUCCEEDED(result)) result = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(execute.put()));
    BSTR target{};
    if (SUCCEEDED(result)) result = execute->get_Path(&target);
    if (FAILED(result)) { error = L"Не удалось проверить задание автозагрузки: " + systemMessage(result); return false; }
    // A portable copy may own the shared task. Uninstall only this installation.
    const bool owned = target && _wcsicmp(target, executablePath().c_str()) == 0;
    SysFreeString(target);
    return !owned || setAutostartEnabled(false, error);
}
