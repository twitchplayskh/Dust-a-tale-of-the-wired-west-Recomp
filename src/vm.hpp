// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "dust.hpp"
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace dust {
struct ScriptError : std::runtime_error {
    uint16_t code;
    ScriptError(uint16_t code, const std::string &message);
};
struct ScriptValue {
    enum class Type { Integer, String, Boolean };
    Type type = Type::Integer;
    int32_t number = 0; // Booleans retain the full original payload, not just 0/1.
    std::string text;
    static ScriptValue integer(int32_t);
    static ScriptValue string(std::string);
    static ScriptValue boolean(int32_t);
};
struct ScriptResult {
    ScriptValue value;
    bool handled = false, passed = true, returned = false;
};
struct ScriptRoute {
    // opcode 0 requests host containment fallback for an absent ordinary call.
    uint16_t opcode = 0;
    uint16_t handlerOpcode = 0; // Nonzero if deferred name was an engine-command token.
    std::string recipient, secondaryRecipient, handler, me, target;
    std::vector<ScriptValue> args;
    bool wantsValue = false;
};
class ScriptProgram {
  public:
    struct Impl;
    explicit ScriptProgram(View bytes);
    explicit ScriptProgram(std::vector<Token> tokens);
    std::vector<std::string> handlers() const;
    void validate(const std::string &handler) const;

  private:
    friend class ScriptVM;
    std::shared_ptr<Impl> impl;
};
class ScriptVM {
  public:
    // Builtins must be explicitly registered. Unknown commands are errors.
    using Builtin = std::function<ScriptValue(const std::vector<ScriptValue> &, bool)>;
    using Router = std::function<ScriptResult(const ScriptRoute &)>;
    struct Impl;
    ScriptVM();
    ScriptVM(const ScriptVM &) = delete;
    ScriptVM &operator=(const ScriptVM &) = delete;
    void setGlobal(const std::string &, ScriptValue);
    ScriptValue global(const std::string &) const;
    bool hasGlobal(const std::string &) const;
    std::unordered_map<std::string, ScriptValue> globals() const;
    void replaceGlobals(std::unordered_map<std::string, ScriptValue>);
    void bind(uint16_t opcode, Builtin);
    // Host lifecycle only: no dispatch may be active when removing a binding.
    void unbind(uint16_t opcode);
    void bindRouter(Router);
    // Host routing entry. Nested calls retain the active budget and stack limits.
    // Result preserves missing-handler and passcode states for containment routing.
    ScriptResult dispatch(const ScriptProgram &, const std::string &handler,
                          const std::vector<ScriptValue> &args = {}, const std::string &me = "",
                          const std::string &target = "", bool wantsValue = false);
    ScriptResult run(const ScriptProgram &, const std::string &handler,
                     const std::vector<ScriptValue> &args = {}, const std::string &me = "",
                     const std::string &target = "");
    // Host safety bounds, not claimed original stack/iteration limits.
    size_t instructionBudget = 1000000, maxCallDepth = 64;

  private:
    std::shared_ptr<Impl> impl;
};
} // namespace dust
