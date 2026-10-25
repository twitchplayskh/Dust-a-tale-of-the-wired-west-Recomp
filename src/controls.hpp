// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <deque>
#include <optional>
#include <string>
namespace dust {
inline std::string gameKeyName(unsigned key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == 32)
        return std::string(1, char(key));
    switch (key) {
    case 0x26:
        return "uparrow";
    case 0x25:
        return "leftarrow";
    case 0x27:
        return "rightarrow";
    case 0x28:
        return "downarrow";
    case 0x0d:
        return "return";
    case 0x09:
        return "tab";
    case 0x08:
        return "backspace";
    case 0x1b:
        return "escape";
    case 0x24:
        return "home";
    case 0x23:
        return "end";
    case 0x21:
        return "pageup";
    case 0x22:
        return "pagedown";
    case 0x2e:
        return "delete";
    default:
        return {};
    }
}
class KeyInput {
  public:
    struct Press {
        unsigned key;
        bool repeat;
    };
    void down(unsigned key) {
        if (key < held.size())
            held[key] = true;
    }
    void up(unsigned key) {
        if (key < held.size())
            held[key] = false;
    }
    void defer(unsigned key, bool repeat) {
        if (repeat) {
            for (const auto &press : queue)
                if (press.key == key && press.repeat)
                    return;
        }
        if (queue.size() < 8)
            queue.push_back({key, repeat});
    }
    std::optional<Press> next() {
        while (!queue.empty()) {
            auto press = queue.front();
            queue.pop_front();
            if (!press.repeat || (press.key < held.size() && held[press.key]))
                return press;
        }
        return {};
    }
    void flush() {
        queue.clear();
    }
    void reset() {
        held.fill(false);
        flush();
    }

  private:
    std::array<bool, 256> held{};
    std::deque<Press> queue;
};
} // namespace dust
