#pragma once

#include <string>

// Проверяет именно поставляемый рядом с приложением MsQuic. Никакой загрузки из
// текущего каталога или PATH: это защищает от подмены DLL.
bool probeQuicRuntime(std::wstring& detail);

