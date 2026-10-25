// SPDX-License-Identifier: GPL-3.0-only
#include "cursor_window.hpp"
#include <stdexcept>
namespace dust {
CursorOutput::CursorOutput(const std::filesystem::path &path) {
    resources = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!resources)
        throw std::runtime_error("cannot read original cursor resource data");
}
CursorOutput::~CursorOutput() {
    for (const auto &[name, cursor] : cursors)
        DestroyCursor(cursor);
    if (resources)
        FreeLibrary(resources);
}
HCURSOR CursorOutput::get(const std::string &name) {
    if (auto it = cursors.find(name); it != cursors.end())
        return it->second;
    std::wstring resource = L"CURS.";
    for (unsigned char c : name) {
        if (c >= 128)
            throw std::runtime_error("non-ASCII cursor resource name");
        resource += wchar_t(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }
    const auto cursor =
        static_cast<HCURSOR>(LoadImageW(resources, resource.c_str(), IMAGE_CURSOR, 32, 32, 0));
    if (!cursor)
        throw std::runtime_error("original cursor resource missing: " + name);
    cursors.emplace(name, cursor);
    return cursor;
}
} // namespace dust
