// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <filesystem>
#include <map>
#include <string>
namespace dust {
// The PE is mapped strictly as resource data. No original entry point or code
// is loaded for execution. Windows reads the shipped cursor planes/hotspots.
class CursorOutput {
  public:
    explicit CursorOutput(const std::filesystem::path &);
    ~CursorOutput();
    CursorOutput(const CursorOutput &) = delete;
    CursorOutput &operator=(const CursorOutput &) = delete;
    HCURSOR get(const std::string &);

  private:
    HMODULE resources = nullptr;
    std::map<std::string, HCURSOR> cursors;
};
} // namespace dust
