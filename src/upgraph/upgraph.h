// MoonUp Upgraph: installs MoonUp' neural rendering *into* a game (no capture, no overlay window,
// with the game's depth buffer) and, for games with DLSS / FSR 2+ / XeSS, AMD FSR 3.1 upscaling and
// frame generation through OptiScaler.
//
//   library scan   Steam, Epic Games, GOG, Ubisoft Connect and folders the user adds
//   game scan      main executable, graphics API (imports, runtime entry points, file name, renderer
//                  modules), bitness, upscaler DLLs, anti-cheat, existing ReShade / OptiScaler
//   install        ReShade (downloaded from reshade.me, or a ReShade_Setup.exe the user picks) +
//                  MoonUpUpgraph.fx + weights + config; optional OptiScaler (GitHub release).
//                  Every file that is replaced is backed up first, everything is recorded in
//                  <exe dir>\_MoonUp_Upgraph\manifest.json
//   remove         deletes what was added and restores the backups
#pragma once
#include <functional>
#include <string>

#include "../common.h"
#include "../third_party/json.hpp"

namespace sw::upgraph {

using json = nlohmann::json;
using Progress = std::function<void(const std::string& stage, double fraction)>;

// Games from the launchers and from 'folders' (array of paths). Fast: no executable inspection.
json ScanLibrary(const json& folders);
// Full inspection of one game folder.
json ScanGame(const std::wstring& dir);
// options: exe (relative or absolute, optional), api (optional override), shader (bool),
// optiscaler (bool), frameGen (bool), dlss5 (bool, NVIDIA RTX), forceFeeder (bool), acceptAntiCheat (bool), preset {tone,color,structure,...}
json Install(const std::wstring& dir, const json& options, const Progress& progress);
json Remove(const std::wstring& dir);
// Updates the slider values in the installed preset (game must be restarted or ReShade reloaded).
json UpdatePreset(const std::wstring& dir, const json& preset);
// Uses a ReShade_Setup*.exe chosen by the user instead of the download.
json UseReShadeSetup(const std::wstring& setupExe);
// Small JPEG/PNG poster as a data: URL (Steam library art), empty when there is none.
// RTX 20/30/40: the user's own GPU-specific DLSS 5 model (nvngx_dlssnr.dll); Clear forgets it.
json UseDlssNr(const std::wstring& file);
json ClearDlssNr();
std::string PosterDataUrl(const std::wstring& file);
// Where downloads are kept (next to MoonUp.exe, never the system drive unless unavoidable).
std::wstring CacheRoot();
// Error code of an exception thrown here ("" when it is not one of ours).
std::string ErrorCode(const std::exception& e);

}  // namespace sw::upgraph
