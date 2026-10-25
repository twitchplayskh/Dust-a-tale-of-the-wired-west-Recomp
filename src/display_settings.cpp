// SPDX-License-Identifier: GPL-3.0-only
#include "display_settings.hpp"
#include <windows.h>
#include <stdexcept>
#include <fstream>
namespace dust {
std::filesystem::path defaultSettingsPath() {
    wchar_t folder[32768]{};
    const auto count = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, DWORD(std::size(folder)));
    if (!count || count >= std::size(folder))
        throw std::runtime_error("LOCALAPPDATA is unavailable");
    return std::filesystem::path(folder) / L"DustRecomp" / L"settings.ini";
}
DisplaySettings loadSettings(const std::filesystem::path &file) {
    DisplaySettings s;
    auto number = [&](const wchar_t *section, const wchar_t *key, int fallback, int low, int high) {
        return std::clamp(int(GetPrivateProfileIntW(section, key, fallback, file.c_str())), low,
                          high);
    };
    s.width = number(L"Display", L"Width", s.width, 512, 16384);
    s.height = number(L"Display", L"Height", s.height, 384, 16384);
    s.fullscreen = number(L"Display", L"Fullscreen", 0, 0, 1) != 0;
    s.integerScaling = number(L"Display", L"IntegerScaling", 0, 0, 1) != 0;
    s.smoothScaling = number(L"Display", L"SmoothScaling", 0, 0, 1) != 0;
    s.restoration.enabled = number(L"Restoration", L"Enabled", s.smoothScaling, 0, 1) != 0;
    s.restoration.deblock = number(L"Restoration", L"Deblock", 40, 0, 100);
    s.restoration.chroma = number(L"Restoration", L"Chroma", 45, 0, 100);
    s.restoration.detail = number(L"Restoration", L"Detail", 15, 0, 35);
    s.restoration.multiscale = number(L"Restoration", L"Multiscale", 1, 0, 1) != 0;
    s.restoration.temporal = number(L"Restoration", L"Temporal", 0, 0, 1) != 0;
    s.restoration.temporalStrength = number(L"Restoration", L"TemporalStrength", 35, 0, 50);
    s.restorationCacheMiB = number(L"Restoration", L"CacheMiB", 128, 16, 512);
    s.volume = number(L"Audio", L"Volume", 100, 0, 100);
    s.captions = number(L"Game", L"Captions", 0, 0, 1) != 0;
    s.skipIntro = number(L"Game", L"SkipIntro", 0, 0, 1) != 0;
    wchar_t root[32768]{};
    GetPrivateProfileStringW(L"Game", L"Root", s.gameRoot.c_str(), root, DWORD(std::size(root)),
                             file.c_str());
    s.gameRoot = root;
    return s;
}
void saveSettings(const std::filesystem::path &file, const DisplaySettings &s) {
    if (!file.parent_path().empty())
        std::filesystem::create_directories(file.parent_path());
    if (!std::filesystem::exists(file)) {
        std::ofstream unicode(file, std::ios::binary);
        unicode.put(char(0xff));
        unicode.put(char(0xfe));
        if (!unicode)
            throw std::runtime_error("cannot create launcher settings");
    }
    auto put = [&](const wchar_t *section, const wchar_t *key, const std::wstring &value) {
        if (!WritePrivateProfileStringW(section, key, value.c_str(), file.c_str()))
            throw std::runtime_error("cannot save launcher settings");
    };
    put(L"Display", L"Width", std::to_wstring(s.width));
    put(L"Display", L"Height", std::to_wstring(s.height));
    put(L"Display", L"Fullscreen", s.fullscreen ? L"1" : L"0");
    put(L"Display", L"IntegerScaling", s.integerScaling ? L"1" : L"0");
    put(L"Display", L"SmoothScaling", s.smoothScaling ? L"1" : L"0");
    put(L"Restoration", L"Enabled", s.restoration.enabled ? L"1" : L"0");
    put(L"Restoration", L"Deblock", std::to_wstring(s.restoration.deblock));
    put(L"Restoration", L"Chroma", std::to_wstring(s.restoration.chroma));
    put(L"Restoration", L"Detail", std::to_wstring(s.restoration.detail));
    put(L"Restoration", L"Multiscale", s.restoration.multiscale ? L"1" : L"0");
    put(L"Restoration", L"Temporal", s.restoration.temporal ? L"1" : L"0");
    put(L"Restoration", L"TemporalStrength", std::to_wstring(s.restoration.temporalStrength));
    put(L"Restoration", L"CacheMiB", std::to_wstring(s.restorationCacheMiB));
    put(L"Audio", L"Volume", std::to_wstring(s.volume));
    put(L"Game", L"Captions", s.captions ? L"1" : L"0");
    put(L"Game", L"SkipIntro", s.skipIntro ? L"1" : L"0");
    put(L"Game", L"Root", s.gameRoot);
}
} // namespace dust
