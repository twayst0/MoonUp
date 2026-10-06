// MoonUp - common helpers shared by the app shell and the engine.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sw {

constexpr const wchar_t* kAppName = L"MoonUp";
constexpr const char* kVersion = "1.0.1";

// ---------------------------------------------------------------- strings
std::string Utf8(const std::wstring& w);
std::string Utf8(const wchar_t* w);
std::wstring Wide(const std::string& s);
std::string HrToString(HRESULT hr);

// ---------------------------------------------------------------- paths
std::wstring ExeDir();                 // folder that contains MoonUp.exe
std::wstring LogDir();                 // logs (program folder\\logs, else the data folder)
std::wstring DataDir();                // %APPDATA%\MoonUp (created on demand)
std::wstring CacheDir();               // %LOCALAPPDATA%\MoonUp (created on demand)

// ---------------------------------------------------------------- time
double NowSeconds();                   // QPC based, monotonic
int64_t QpcNow();
double QpcToSeconds(int64_t ticks);

// ---------------------------------------------------------------- logging
void LogInit(const wchar_t* name = L"moonup");
void Log(const char* fmt, ...);
#define SW_LOG(...) ::sw::Log(__VA_ARGS__)

// Returns false and logs when hr failed.
bool Check(HRESULT hr, const char* what);

// Scoped high resolution timer period (1 ms) for precise frame pacing.
struct TimerResolution {
    TimerResolution();
    ~TimerResolution();
};

// Precise sleep that combines a high resolution waitable timer with a short spin.
void PreciseSleepUntil(double targetSeconds);

}  // namespace sw
