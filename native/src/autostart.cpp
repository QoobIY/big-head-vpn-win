#include "autostart.h"

#include <windows.h>
#include <taskschd.h>
#include <oleauto.h>

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
    return enabled == VARIANT_TRUE;
}

bool setAutostartEnabled(bool enabled, std::wstring& error) {
    error.clear();
    ComSession session;
    if (!session.ready()) { error = L"Не удалось инициализировать Windows COM"; return false; }
    ComObject<ITaskService> service; ComObject<ITaskFolder> root;
    if (!openScheduler(service, root, error)) return false;
    BString name(TASK_NAME);
    if (!enabled) {
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
    if (SUCCEEDED(task->get_Settings(settings.put()))) {
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    }
    ComObject<ITriggerCollection> triggers;
    ComObject<ITrigger> trigger;
    if (FAILED(task->get_Triggers(triggers.put())) || FAILED(triggers->Create(TASK_TRIGGER_LOGON, trigger.put()))) {
        error = L"Не удалось создать триггер входа в Windows"; return false;
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
    if (FAILED(execute->put_Path(executable)) || FAILED(execute->put_Arguments(arguments))) {
        error = L"Не удалось записать путь приложения в автозагрузку"; return false;
    }
    VARIANT empty; VariantInit(&empty);
    ComObject<IRegisteredTask> registered;
    result = root->RegisterTaskDefinition(name, task.get(), TASK_CREATE_OR_UPDATE, empty, empty,
        TASK_LOGON_INTERACTIVE_TOKEN, empty, registered.put());
    if (FAILED(result)) { error = L"Не удалось включить автозагрузку: " + systemMessage(result); return false; }
    return true;
}
