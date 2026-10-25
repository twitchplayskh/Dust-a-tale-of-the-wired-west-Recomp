// SPDX-License-Identifier: GPL-3.0-only
// Directory layouts informed by pinned dreamREfactory stg.ts/cst.ts/shp.ts.
#include "contexts.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace dust {
namespace {
void need(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
size_t count(View data, size_t at, size_t first, size_t stride) {
    auto n = u32(data, at);
    need(first <= data.size() && n <= (data.size() - first) / stride,
         "resource directory exceeds container");
    return n;
}
void reference(const File &f, uint32_t ref, bool optional = false) {
    if (!ref && optional)
        return;
    (void)f.at(ref);
}
ContextState state(const File &file, uint32_t ref, std::string name) {
    ContextState out;
    out.name = std::move(name);
    out.logicRef = ref;
    auto data = file.at(ref);
    const auto n = count(data, 114, 118, 44);
    for (size_t i = 0; i < n; ++i) {
        const auto at = 118 + i * 44;
        ContextFrame frame;
        frame.spriteRef = u32(data, at);
        reference(file, frame.spriteRef);
        frame.step = i16(data, at + 8);
        frame.direction = i16(data, at + 10);
        frame.height = i16(data, at + 22);
        frame.width = i16(data, at + 24);
        frame.offsetY = i16(data, at + 26);
        frame.offsetX = i16(data, at + 28);
        frame.angle = i16(data, at + 40);
        frame.scale = i16(data, at + 42);
        out.frames.push_back(frame);
    }
    // Retain the authored play-list as metadata. Selection/validation depends
    // on whether the caller plays cast step groups or shop frame variants.
    int playCount = i16(data, 112);
    need(playCount >= 0 && playCount <= 33, "invalid resource play-list length");
    for (int i = 0; i < playCount; ++i)
        out.playOrder.push_back(i16(data, 46 + size_t(i) * 2) - 1);
    return out;
}
} // namespace
const ContextState *ContextEntry::state(const std::string &key) const {
    const auto wanted = lower(key);
    for (const auto &s : states)
        if (lower(s.name) == wanted)
            return &s;
    return nullptr;
}
const ContextEntry *ResourceContexts::find(const std::string &key) const {
    const auto wanted = lower(key);
    for (const auto &e : entries)
        if (lower(e.name) == wanted)
            return &e;
    return nullptr;
}
View ResourceContexts::script(const std::string &key) const {
    const auto *entry = find(key);
    need(entry != nullptr, "resource script context not found");
    return entry->scriptRef ? file.at(entry->scriptRef) : View{};
}
View ResourceContexts::main() const {
    return mainScriptRef ? file.at(mainScriptRef) : View{};
}
ResourceContexts ResourceContexts::open(const std::filesystem::path &path, ContextKind kind) {
    ResourceContexts out;
    out.kind = kind;
    out.file = File::open(path);
    auto header = out.file.at(0);
    need(u32(header, 0) == 0x10000, "resource context requires DreamFactory 1");
    need(header.size() >= 2084, "truncated resource palette");
    for (size_t i = 0; i < 256; ++i) {
        const auto at = 36 + i * 8;
        out.palette[i] =
            uint32_t(header[at + 3]) << 16 | uint32_t(header[at + 5]) << 8 | header[at + 7];
    }
    out.palette[0] = 0;
    out.palette[255] = 0xffffff;
    if (kind == ContextKind::Stage) {
        out.width = i16(header, 28);
        out.height = i16(header, 30);
        out.mainScriptRef = u32(header, 32);
        out.name = pstr(header, 2084, 15);
        const auto n = count(header, 2100, 2104, 28);
        for (size_t i = 0; i < n; ++i) {
            const auto at = 2104 + i * 28;
            ContextEntry e;
            e.scriptRef = u32(header, at);
            e.frameRef = u32(header, at + 4);
            e.logicRef = u32(header, at + 8);
            e.name = pstr(header, at + 12, 15);
            e.width = out.width;
            e.height = out.height;
            reference(out.file, e.scriptRef, true);
            reference(out.file, e.frameRef, true);
            if (e.logicRef) {
                auto logic = out.file.at(e.logicRef);
                const auto regions = count(logic, 0, 4, 32);
                for (size_t r = 0; r < regions; ++r) {
                    const auto pos = 4 + r * 32;
                    ContextRegion region;
                    region.top = i16(logic, pos + 4);
                    region.left = i16(logic, pos + 6);
                    region.bottom = i16(logic, pos + 8);
                    region.right = i16(logic, pos + 10);
                    region.scriptRef = u32(logic, pos + 12);
                    region.name = pstr(logic, pos + 16, 15);
                    reference(out.file, region.scriptRef, true);
                    e.regions.push_back(std::move(region));
                }
            }
            out.entries.push_back(std::move(e));
        }
    } else {
        out.mainScriptRef = u32(header, 2340);
        out.name = pstr(header, 2344, 15);
        const auto n = count(header, 2360, 2364, 16);
        for (size_t i = 0; i < n; ++i) {
            ContextEntry e;
            e.logicRef = u32(header, 2364 + i * 16);
            auto logic = out.file.at(e.logicRef);
            e.scriptRef = u32(logic, 38);
            e.name = pstr(logic, 42, 47);
            e.logicPrefix.assign(logic.begin(), logic.begin() + 38);
            reference(out.file, e.scriptRef, true);
            const auto states = count(logic, 90, 94, 32);
            for (size_t s = 0; s < states; ++s) {
                const auto at = 94 + s * 32;
                e.states.push_back(dust::state(out.file, u32(logic, at), pstr(logic, at + 16, 15)));
            }
            out.entries.push_back(std::move(e));
        }
    }
    reference(out.file, out.mainScriptRef, true);
    return out;
}
} // namespace dust
