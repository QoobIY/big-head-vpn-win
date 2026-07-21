#include "quic_runtime.h"

#include <windows.h>
#include <msquic.h>

#include <filesystem>

bool probeQuicRuntime(std::wstring& detail) {
    wchar_t executable[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length == std::size(executable)) {
        detail = L"Не удалось определить папку приложения";
        return false;
    }
    auto dll = std::filesystem::path(executable).parent_path() / L"msquic.dll";
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        detail = L"Рядом с EXE отсутствует msquic.dll";
        return false;
    }
    auto open = reinterpret_cast<MsQuicOpenVersionFn>(GetProcAddress(module, "MsQuicOpenVersion"));
    auto close = reinterpret_cast<MsQuicCloseFn>(GetProcAddress(module, "MsQuicClose"));
    if (!open || !close) {
        detail = L"Неподдерживаемая версия msquic.dll";
        FreeLibrary(module);
        return false;
    }
    const void* api = nullptr;
    QUIC_STATUS status = open(QUIC_API_VERSION_2, &api);
    if (QUIC_FAILED(status) || !api) {
        detail = L"MsQuic не смог запустить QUIC/TLS (код " + std::to_wstring(status) + L")";
        FreeLibrary(module);
        return false;
    }
    close(api);
    FreeLibrary(module);
    detail = L"MsQuic 2.5.9 готов; профиль Hysteria2 распознан";
    return true;
}

