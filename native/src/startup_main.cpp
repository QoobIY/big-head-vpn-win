#include "autostart.h"
#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring error;
    if (runAutostartTask(error)) return 0;
    // Avoid a blocking dialog during logon; report failure to Windows' debugger.
    OutputDebugStringW((L"Big Head VPN startup: " + error).c_str());
    return 1;
}
