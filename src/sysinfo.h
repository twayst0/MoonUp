// System information for the UI and the advisor: GPUs, displays, CPU, memory, OS, windows.
#pragma once
#include "third_party/json.hpp"

#include <windows.h>

namespace sw {

nlohmann::json CollectSystemInfo();
nlohmann::json ListWindows(HWND exclude);
std::string WindowExeName(HWND h);
std::string WindowTitle(HWND h);
// Encodes an HICON as a PNG data URL (used for window icons in the picker).
std::string IconToDataUrl(HICON icon, int size);

}  // namespace sw
