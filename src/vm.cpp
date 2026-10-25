// SPDX-License-Identifier: GPL-3.0-only
// Typed DreamFactory 1 core, adapted from pinned GPL parser.ts/interp.ts.
#include "vm.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <unordered_set>

namespace dust {
ScriptError::ScriptError(uint16_t c, const std::string &m) : std::runtime_error(m), code(c) {}
ScriptValue ScriptValue::integer(int32_t n) {
    return {Type::Integer, n, {}};
}
ScriptValue ScriptValue::string(std::string s) {
    if (s.size() > 255)
        throw ScriptError(0x1a, "string exceeds 255 bytes");
    return {Type::String, 0, std::move(s)};
}
ScriptValue ScriptValue::boolean(int32_t n) {
    return {Type::Boolean, n, {}};
}
namespace {
std::string fold(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c + ('a' - 'A'));
    return s;
}
[[noreturn]] void fail(uint16_t code, const char *why) {
    throw ScriptError(code, why);
}
void require(bool b, uint16_t code, const char *why) {
    if (!b)
        fail(code, why);
}
int32_t integer(const ScriptValue &v) {
    require(v.type == ScriptValue::Type::Integer, 0xe, "integer required");
    return v.number;
}
bool condition(const ScriptValue &v) {
    require(v.type == ScriptValue::Type::Boolean, 0xe, "boolean condition required");
    return v.number != 0;
}
bool equal(const ScriptValue &a, const ScriptValue &b) {
    if (a.type != b.type)
        return false;
    return a.type == ScriptValue::Type::String ? fold(a.text) == fold(b.text)
                                               : a.number == b.number;
}
int32_t wrap(uint32_t n) {
    return std::bit_cast<int32_t>(n);
}
int precedence(uint16_t op) {
    switch (op) {
    case 8006:
        return 1;
    case 8005:
        return 2;
    case 8008:
    case 8009:
        return 3;
    case 8010:
    case 8011:
    case 8012:
    case 8013:
        return 4;
    case 8007:
        return 5;
    case 8001:
    case 8002:
        return 6;
    case 8003:
    case 8004:
        return 7;
    default:
        return 0;
    }
}
ScriptValue binary(uint16_t op, const ScriptValue &a, const ScriptValue &b) {
    using V = ScriptValue;
    if (op == 8008 || op == 8009)
        return V::boolean(op == 8008 ? equal(a, b) : !equal(a, b));
    if (op == 8005 || op == 8006) {
        require(a.type == V::Type::Boolean && b.type == V::Type::Boolean, 0xe,
                "boolean operands required");
        return V::boolean(op == 8005 ? a.number & b.number : a.number | b.number);
    }
    if (op == 8007) {
        require(a.type == V::Type::String && b.type == V::Type::String, 0xe,
                "string operands required");
        return V::string(a.text + b.text);
    }
    int32_t x = integer(a), y = integer(b);
    switch (op) {
    case 8001:
        return V::integer(wrap(uint32_t(x) + uint32_t(y)));
    case 8002:
        return V::integer(wrap(uint32_t(x) - uint32_t(y)));
    case 8003:
        return V::integer(wrap(uint32_t(x) * uint32_t(y)));
    case 8004:
        require(y != 0, 0x37, "division by zero");
        if (x == std::numeric_limits<int32_t>::min() && y == -1)
            throw std::overflow_error(
                "original IDIV overflow: processor-exception behavior is not emulated");
        return V::integer(wrap(uint32_t(int64_t(x) / int64_t(y))));
    case 8010:
        return V::boolean(x > y);
    case 8011:
        return V::boolean(x < y);
    case 8012:
        return V::boolean(x >= y);
    case 8013:
        return V::boolean(x <= y);
    default:
        fail(1, "unknown binary operator");
    }
}
struct Expr;
using E = std::shared_ptr<Expr>;
struct Expr {
    enum Kind { Literal, Variable, Me, Target, Unary, Binary, Call } kind = Literal;
    ScriptValue literal;
    std::string name;
    uint16_t op = 0;
    std::vector<E> args;
};
struct Stmt;
using Block = std::vector<Stmt>;
struct Case {
    E match;
    Block body;
};
struct Stmt {
    enum Kind {
        Declaration,
        Assignment,
        Call,
        If,
        Switch,
        While,
        For,
        Return,
        Exit,
        Pass,
        Fault
    } kind = Fault;
    uint16_t op = 0;
    std::string name;
    std::vector<std::string> names;
    E expr, limit, step;
    Block body, alternative;
    std::vector<Case> cases;
};
struct Handler {
    std::vector<std::string> params;
    Block body;
};
class Parser {
    const std::vector<Token> &t;
    size_t p, end;
    uint16_t op() const {
        return p < end ? t[p].cmd : 0;
    }
    bool take(uint16_t c) {
        if (op() == c) {
            ++p;
            return true;
        }
        return false;
    }
    void expect(uint16_t c, uint16_t err = 2) {
        require(take(c), err, "unexpected script token");
    }
    std::string name() {
        require(op() == 5, 0xe, "variable name expected");
        return fold(t[p++].text);
    }
    void breaks() {
        while (op() == 6)
            ++p;
    }
    void line() {
        require(p == end || op() == 6 || op() == 4004, 0x1b, "expected line break");
        breaks();
    }
    E primary() {
        require(p < end, 0xe, "missing expression");
        auto token = t[p++];
        auto e = std::make_shared<Expr>();
        if (token.cmd == 4) {
            e->literal = ScriptValue::integer(wrap(token.info));
            return e;
        }
        if (token.cmd == 3) {
            e->literal = ScriptValue::string(token.text);
            return e;
        }
        if (token.cmd == 4021 || token.cmd == 4022) {
            e->literal = ScriptValue::boolean(token.cmd == 4021);
            return e;
        }
        if (token.cmd == 4026 || token.cmd == 4027) {
            e->kind = token.cmd == 4026 ? Expr::Me : Expr::Target;
            return e;
        }
        if (token.cmd == 8002 || token.cmd == 4023) {
            e->kind = Expr::Unary;
            e->op = token.cmd;
            e->args.push_back(primary());
            return e;
        }
        if (token.cmd == 4018) {
            auto result = expression();
            expect(4019);
            return result;
        }
        if (token.cmd == 12009) {
            // ALT31 0x425d30 consumes a RAW effect keyword, not an expression
            // or a general numeric constant. The host receives its exact opcode.
            e->kind = Expr::Call;
            e->op = token.cmd;
            expect(4018);
            require(p < end && t[p].cmd >= 24001 && t[p].cmd <= 24014, 0xe,
                    "visualeffect requires an original effect keyword");
            auto effect = std::make_shared<Expr>();
            effect->literal = ScriptValue::integer(t[p++].cmd);
            e->args.push_back(effect);
            expect(4020, 0x1c);
            e->args.push_back(expression());
            expect(4019);
            return e;
        }
        if (token.cmd == 5 || (token.cmd >= 12000 && token.cmd < 24000)) {
            e->name = fold(token.text);
            e->op = token.cmd == 5 ? 0 : token.cmd;
            e->kind = Expr::Variable;
            if (take(4018)) {
                e->kind = Expr::Call;
                if (!take(4019)) {
                    do {
                        e->args.push_back(expression());
                    } while (take(4020));
                    expect(4019);
                }
            } else
                require(token.cmd == 5, 0xe, "command requires parentheses");
            return e;
        }
        throw ScriptError(0xe, "invalid expression operand opcode " + std::to_string(token.cmd) +
                                   " at token " + std::to_string(p - 1));
    }
    E expression(int min = 1) {
        auto left = primary();
        for (;;) {
            int prec = precedence(op());
            if (prec < min)
                break;
            uint16_t code = t[p++].cmd;
            auto right = expression(prec + 1);
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Binary;
            e->op = code;
            e->args = {left, right};
            left = e;
        }
        return left;
    }
    Block block(std::initializer_list<uint16_t> closers) {
        Block out;
        breaks();
        while (p < end && std::find(closers.begin(), closers.end(), op()) == closers.end()) {
            out.push_back(statement());
            breaks();
        }
        return out;
    }
    Stmt statement() {
        Stmt s;
        uint16_t c = op();
        if (c == 4007) {
            // CHECKERS.PRP rowcol2move ends with an unreachable stray endif.
            // Keep it as a runtime fault, so a preceding return can still run.
            ++p;
            s.kind = Stmt::Fault;
            s.op = c;
            line();
            return s;
        }
        if (c == 4002 || c == 4003 || c == 4028 || c == 4029) {
            ++p;
            s.kind = Stmt::Declaration;
            s.op = c;
            do {
                s.names.push_back(name());
            } while (take(4020));
            line();
            return s;
        }
        if (c == 4005 || c == 4025) {
            ++p;
            s.kind = c == 4005 ? Stmt::Exit : Stmt::Pass;
            if (take(4018))
                expect(4019);
            line();
            return s;
        }
        if (c == 4024) {
            ++p;
            s.kind = Stmt::Return;
            if (op() != 6 && p < end)
                s.expr = expression();
            line();
            return s;
        }
        if (c == 4006) {
            ++p;
            s.kind = Stmt::If;
            s.expr = expression();
            line();
            s.body = block({4008, 4007});
            if (take(4008)) {
                line();
                s.alternative = block({4007});
            }
            expect(4007, 0x1d);
            line();
            return s;
        }
        if (c == 4016) {
            ++p;
            s.kind = Stmt::While;
            s.expr = expression();
            line();
            s.body = block({4017});
            expect(4017, 0x25);
            line();
            return s;
        }
        if (c == 4012) {
            ++p;
            s.kind = Stmt::For;
            s.name = name();
            expect(8008);
            s.expr = expression();
            expect(4013);
            s.limit = expression();
            if (take(4014))
                s.step = expression();
            line();
            s.body = block({4015});
            expect(4015, 0x1e);
            line();
            return s;
        }
        if (c == 4009) {
            ++p;
            s.kind = Stmt::Switch;
            s.expr = expression();
            line();
            auto dead = block({4011, 4010, 4007});
            (void)dead;
            while (take(4011)) {
                Case arm;
                arm.match = expression();
                line();
                arm.body = block({4011, 4010, 4007});
                s.cases.push_back(std::move(arm));
            }
            // EXTRA.CST kidgangloop ships an endif in place of endswitch.
            // Each authored arm exits before that unreachable closing token.
            if (!take(4007))
                expect(4010, 0x1f);
            line();
            return s;
        }
        if (c == 5 && p + 1 < end && t[p + 1].cmd == 8008) {
            s.kind = Stmt::Assignment;
            s.name = name();
            ++p;
            s.expr = expression();
            line();
            return s;
        }
        s.kind = Stmt::Call;
        s.expr = primary();
        require(s.expr->kind == Expr::Call, 1, "statement must be assignment or call");
        line();
        return s;
    }

  public:
    Parser(const std::vector<Token> &tokens, size_t start, size_t finish)
        : t(tokens), p(start), end(finish) {}
    Handler handler() {
        Handler h;
        expect(4018);
        if (!take(4019)) {
            do {
                h.params.push_back(name());
            } while (take(4020));
            expect(4019);
        }
        line();
        h.body = block({4004});
        return h;
    }
};
struct Signal {
    enum Kind { Normal, Return, Exit, Pass } kind = Normal;
    ScriptValue value;
};
struct Frame {
    std::unordered_map<std::string, ScriptValue> locals;
    std::string me, target, handler;
    int wants = -1;
};
} // namespace
struct ScriptProgram::Impl {
    std::vector<Token> tokens;
    std::unordered_map<std::string, std::pair<size_t, size_t>> positions;
    mutable std::unordered_map<std::string, Handler> compiled;
    explicit Impl(std::vector<Token> input) {
        for (size_t i = 0; i < input.size(); ++i) {
            // Compiled source comments can contain keyword tokens (including
            // 'code'). They are not handler declarations or executable text.
            if (i + 1 < input.size() && input[i].cmd == 8004 && input[i + 1].cmd == 8004) {
                while (i < input.size() && input[i].cmd != 6)
                    ++i;
                if (i == input.size())
                    break;
            }
            if (input[i].cmd != 1)
                tokens.push_back(std::move(input[i]));
        }
        for (size_t i = 0; i + 1 < tokens.size(); ++i)
            if (tokens[i].cmd == 4001) {
                require(tokens[i + 1].cmd == 5, 0xe, "code name expected");
                size_t end = i + 2;
                while (end < tokens.size() && tokens[end].cmd != 4001 && tokens[end].cmd != 4004)
                    ++end;
                positions[fold(tokens[i + 1].text)] = {i + 2, end};
            }
    }
    const Handler *handler(const std::string &name) const {
        auto pos = positions.find(name);
        if (pos == positions.end())
            return nullptr;
        auto h = compiled.find(name);
        if (h == compiled.end()) {
            try {
                h = compiled
                        .emplace(name,
                                 Parser(tokens, pos->second.first, pos->second.second).handler())
                        .first;
            } catch (const ScriptError &error) {
                throw ScriptError(error.code, "handler " + name + ": " + error.what());
            }
        }
        return &h->second;
    }
};
static std::vector<Token> originalGameplayTokens(View bytes) {
    uint64_t fingerprint = 14695981039346656037ull;
    for (auto b : bytes)
        fingerprint = (fingerprint ^ b) * 1099511628211ull;
    const bool fear = fingerprint == 0x6679987640291e6full || fingerprint == 0x99897a6605535731ull;
    const bool buick = fingerprint == 0x7d3946f0cfd99f33ull;
    const bool dell = fingerprint == 0xf040927343d09998ull || fingerprint == 0x74814415a67718e4ull;
    const bool mez = fingerprint == 0x2cc70800b99eb800ull;
    const bool gus = fingerprint == 0x516d1bee0f535898ull;
    auto tokens = script(bytes);
    if (!fear && !buick && !dell && !mez && !gus)
        return tokens;
    // Exact-blob compatibility repairs for authored typos. Original files are
    // never patched. Synthetic/custom scripts retain strict token semantics.
    std::vector<Token> repaired;
    std::string handler;
    for (size_t i = 0; i < tokens.size(); ++i) {
        auto token = tokens[i];
        if (token.cmd == 4001 && i + 1 < tokens.size())
            handler = fold(tokens[i + 1].text);
        if (fear && token.cmd == 5 && fold(token.text) == "exitode")
            token.cmd = 4005;
        if (buick && token.cmd == 5 && fold(token.text) == "cased")
            token.cmd = 4011;
        if (mez && token.cmd == 5 && fold(token.text) == "is" && i > 0 && i + 1 < tokens.size() &&
            tokens[i - 1].cmd == 5 && fold(tokens[i - 1].text) == "clock" &&
            tokens[i + 1].cmd == 8010)
            continue;
        if (dell && token.cmd == 4004 && handler == "subrack5") {
            repaired.push_back({4010, 0, {}});
            repaired.push_back({6, 0, {}});
        }
        // buydrink is used as a value. Escape must return the same stop
        // sentinel as its Goodbye/broke paths instead of exiting without one.
        if (gus && handler == "buydrink" && token.cmd == 4005) {
            repaired.push_back({4024, 0, {}});
            repaired.push_back({4, 1, {}});
            continue;
        }
        repaired.push_back(std::move(token));
    }
    return repaired;
}
ScriptProgram::ScriptProgram(View bytes) : ScriptProgram(originalGameplayTokens(bytes)) {}
ScriptProgram::ScriptProgram(std::vector<Token> tokens)
    : impl(std::make_shared<Impl>(std::move(tokens))) {}
std::vector<std::string> ScriptProgram::handlers() const {
    std::vector<std::string> result;
    for (auto &[name, position] : impl->positions)
        result.push_back(name);
    std::sort(result.begin(), result.end());
    return result;
}
void ScriptProgram::validate(const std::string &name) const {
    require(impl->handler(fold(name)) != nullptr, 1, "handler not found");
}
struct ScriptVM::Impl {
    std::unordered_map<std::string, ScriptValue> globals;
    std::unordered_map<uint16_t, Builtin> builtins;
    Router router;
    size_t remaining = 0, depth = 0, forOpen = 0, whileOpen = 0;
    void charge() {
        if (!remaining)
            throw std::runtime_error("native script instruction budget exhausted");
        --remaining;
    }
    ScriptValue read(const std::string &n, Frame &f) {
        auto l = f.locals.find(n);
        if (l != f.locals.end())
            return l->second;
        auto g = globals.find(n);
        if (g != globals.end())
            return g->second;
        fail(9, "undeclared variable read");
    }
    ScriptValue &variable(const std::string &n, Frame &f) {
        auto l = f.locals.find(n);
        if (l != f.locals.end())
            return l->second;
        auto g = globals.find(n);
        if (g != globals.end())
            return g->second;
        fail(9, "undeclared variable assignment");
    }
    ScriptValue eval(const E &e, const ScriptProgram::Impl &p, Frame &f, size_t maxDepth) {
        charge();
        using V = ScriptValue;
        switch (e->kind) {
        case Expr::Literal:
            return e->literal;
        case Expr::Me:
            return V::string(f.me);
        case Expr::Target:
            return V::string(f.target);
        case Expr::Variable:
            return read(e->name, f);
        case Expr::Unary: {
            auto v = eval(e->args[0], p, f, maxDepth);
            if (e->op == 4023) {
                require(v.type == V::Type::Boolean, 0xe, "not requires boolean");
                return V::boolean(v.number ^ 1);
            }
            return V::integer(wrap(0u - uint32_t(integer(v))));
        }
        case Expr::Binary: {
            auto a = eval(e->args[0], p, f, maxDepth);
            auto b = eval(e->args[1], p, f, maxDepth);
            return binary(e->op, a, b);
        }
        case Expr::Call:
            return call(e, p, f, maxDepth, true);
        }
        fail(1, "invalid expression node");
    }
    ScriptValue call(const E &e, const ScriptProgram::Impl &p, Frame &f, size_t maxDepth,
                     bool wants) {
        charge();
        // These are syntax forms: their final call is a message, not an
        // expression evaluated against the sending object's script.
        bool implicit = false, routed = false, routeValue = false, floor = false, button = false;
        switch (e->op) {
        case 12070:
        case 12081:
        case 12067:
            implicit = routed = true;
            break;
        case 20094:
        case 20095:
        case 20091:
            implicit = routed = routeValue = true;
            break;
        case 12066:
            routed = floor = true;
            break;
        case 20090:
            routed = floor = routeValue = true;
            break;
        case 12068:
            routed = button = true;
            break;
        case 20092:
            routed = button = routeValue = true;
            break;
        case 12016:
        case 12034:
        case 12045:
        case 12048:
        case 12055:
        case 12059:
        case 12069:
            routed = true;
            break;
        case 20084:
        case 20085:
        case 20086:
        case 20087:
        case 20088:
        case 20089:
        case 20093:
            routed = routeValue = true;
            break;
        }
        if (routed) {
            require(wants == routeValue, 1, "sendto form unavailable in this dispatch context");
            require(bool(router), 1, "unimplemented engine message router");
            require(e->args.size() == (implicit ? 1u
                                       : button ? 3u
                                                : 2u),
                    2, "sendto argument count mismatch");
            const auto &deferred = e->args.back();
            require(deferred->kind == Expr::Call, 1, "sendto requires a deferred handler call");
            ScriptRoute request;
            request.opcode = e->op;
            request.handler = deferred->name;
            request.handlerOpcode = deferred->op;
            request.me = f.me;
            request.target = f.target;
            request.wantsValue = routeValue;
            if (!implicit) {
                auto recipient = eval(e->args.front(), p, f, maxDepth);
                // Original button wrapper evaluates both address expressions
                // before checking their string tags, through 0x417070.
                ScriptValue secondary;
                if (button)
                    secondary = eval(e->args[1], p, f, maxDepth);
                if (floor)
                    request.recipient = std::to_string(integer(recipient));
                else {
                    require(recipient.type == ScriptValue::Type::String, 0xe,
                            "sendto recipient must be a string");
                    request.recipient = recipient.text;
                }
                if (button) {
                    require(secondary.type == ScriptValue::Type::String, 0xe,
                            "sendtobutton button name must be a string");
                    request.secondaryRecipient = secondary.text;
                }
            }
            for (const auto &argument : deferred->args)
                request.args.push_back(eval(argument, p, f, maxDepth));
            auto result = router(request);
            if (routeValue)
                require(result.returned, 6, "message handler did not return a requested value");
            return result.value;
        }
        if (e->op) {
            bool valid =
                wants ? ((e->op >= 16001 && e->op <= 16053) || (e->op >= 20001 && e->op <= 20108))
                      : ((e->op >= 12001 && e->op <= 12088) || (e->op >= 16001 && e->op <= 16053));
            require(valid, 1, "command unavailable in this dispatch context");
            if (e->op != 16030 && !builtins.contains(e->op))
                throw ScriptError(1, "unimplemented engine command opcode " +
                                         std::to_string(e->op) + " in " + f.handler +
                                         " (me=" + f.me + ", target=" + f.target + ")");
        }
        std::vector<ScriptValue> args;
        for (auto &a : e->args)
            args.push_back(eval(a, p, f, maxDepth));
        if (e->op == 16030) {
            require(args.size() == 1 || args.size() == 2, 2, "variable argument count");
            require(args[0].type == ScriptValue::Type::String, 14, "variable name requires text");
            auto &value = variable(fold(args[0].text), f);
            if (args.size() == 2)
                value = args[1];
            return value;
        }
        if (e->op)
            return builtins.at(e->op)(args, wants);
        if (!p.positions.contains(e->name)) {
            require(bool(router), 4, "unknown script handler");
            ScriptRoute request;
            request.handler = e->name;
            request.args = std::move(args);
            request.me = f.me;
            request.target = f.target;
            request.wantsValue = wants;
            auto result = router(request);
            require(result.handled, 4, "unknown script handler after containment fallback");
            if (wants)
                require(result.returned, 6, "fallback handler did not return a requested value");
            return result.value;
        }
        return invoke(p, e->name, args, f.me, f.target, maxDepth, true, wants).value;
    }
    Signal block(const Block &b, const ScriptProgram::Impl &p, Frame &f, size_t maxDepth) {
        for (auto &s : b) {
            auto signal = statement(s, p, f, maxDepth);
            if (signal.kind != Signal::Normal)
                return signal;
        }
        return {};
    }
    Signal statement(const Stmt &s, const ScriptProgram::Impl &p, Frame &f, size_t maxDepth) {
        charge();
        using V = ScriptValue;
        switch (s.kind) {
        case Stmt::Declaration:
            for (auto &n : s.names) {
                if (s.op == 4002)
                    globals.try_emplace(n, V::integer(0));
                else if (s.op == 4003)
                    f.locals.try_emplace(n, V::integer(0));
                else if (s.op == 4028)
                    f.locals.erase(n);
                else
                    globals.erase(n);
            }
            return {};
        case Stmt::Assignment: {
            variable(s.name, f);
            auto value = eval(s.expr, p, f, maxDepth);
            variable(s.name, f) = std::move(value);
            return {};
        }
        case Stmt::Call:
            call(s.expr, p, f, maxDepth, false);
            return {};
        case Stmt::Return:
            require(f.wants != 0, 5, "return in a procedure called for no value");
            require(s.expr || f.wants != 1, 0xe, "return has no requested value");
            return {Signal::Return, s.expr ? eval(s.expr, p, f, maxDepth) : V::integer(0)};
        case Stmt::Exit:
            return {Signal::Exit, {}};
        case Stmt::Pass:
            return {Signal::Pass, {}};
        case Stmt::If:
            return block(condition(eval(s.expr, p, f, maxDepth)) ? s.body : s.alternative, p, f,
                         maxDepth);
        case Stmt::Switch: {
            auto subject = eval(s.expr, p, f, maxDepth);
            require(subject.type != V::Type::Boolean, 0xe, "switch requires integer or string");
            for (size_t i = 0; i < s.cases.size(); ++i) {
                auto match = eval(s.cases[i].match, p, f, maxDepth);
                require(subject.type == match.type, 0xe, "switch case type mismatch");
                if (equal(subject, match)) {
                    while (i + 1 < s.cases.size() && s.cases[i].body.empty())
                        ++i;
                    return block(s.cases[i].body, p, f, maxDepth);
                }
            }
            return {};
        }
        case Stmt::While: {
            bool entered = false;
            try {
                while (condition(eval(s.expr, p, f, maxDepth))) {
                    charge();
                    if (!entered) {
                        entered = true;
                        require(++whileOpen < 20, 3, "while nesting limit");
                    }
                    auto sig = block(s.body, p, f, maxDepth);
                    if (sig.kind != Signal::Normal) {
                        --whileOpen;
                        return sig;
                    }
                }
            } catch (...) {
                if (entered)
                    --whileOpen;
                throw;
            }
            if (entered)
                --whileOpen;
            return {};
        }
        case Stmt::For: {
            auto from = integer(eval(s.expr, p, f, maxDepth)),
                 to = integer(eval(s.limit, p, f, maxDepth));
            auto step = s.step ? integer(eval(s.step, p, f, maxDepth)) : 1;
            f.locals[s.name] = V::integer(from);
            if ((step > 0 && from > to) || (step < 0 && from < to))
                return {};
            require(forOpen + 1 < 20, 3, "for nesting limit");
            ++forOpen;
            try {
                for (;;) {
                    charge();
                    auto sig = block(s.body, p, f, maxDepth);
                    if (sig.kind != Signal::Normal) {
                        --forOpen;
                        return sig;
                    }
                    auto next = wrap(uint32_t(integer(read(s.name, f))) + uint32_t(step));
                    f.locals[s.name] = V::integer(next);
                    if (step >= 0 ? next > to : next < to)
                        break;
                }
            } catch (...) {
                --forOpen;
                throw;
            }
            --forOpen;
            return {};
        }
        case Stmt::Fault:
            fail(0x1d, "unmatched block closing statement");
        default:
            fail(1, "unsupported statement");
        }
    }
    ScriptResult invoke(const ScriptProgram::Impl &p, const std::string &handler,
                        const std::vector<ScriptValue> &args, const std::string &me,
                        const std::string &target, size_t maxDepth, bool internal = false,
                        bool wants = false, bool preservePass = false) {
        auto h = p.handler(handler);
        if (!h)
            return {{}, false, true, false};
        require(depth < maxDepth, 0x2c, "native script call-depth limit");
        if (args.size() != h->params.size())
            fail(args.size() > h->params.size() ? 2
                 : args.empty()                 ? 0xe
                                                : 0x1c,
                 "handler argument count mismatch");
        Frame f;
        f.handler = handler;
        f.me = me;
        f.target = target;
        f.wants = internal ? (wants ? 1 : 0) : -1;
        for (size_t i = 0; i < h->params.size(); ++i)
            f.locals[h->params[i]] = args[i];
        ++depth;
        Signal sig;
        try {
            sig = block(h->body, p, f, maxDepth);
        } catch (...) {
            --depth;
            throw;
        }
        --depth;
        if (wants && sig.kind != Signal::Return && !(preservePass && sig.kind == Signal::Pass))
            fail(6, "handler did not return a requested value");
        return {sig.kind == Signal::Return ? sig.value : ScriptValue::integer(0), true,
                sig.kind == Signal::Pass, sig.kind == Signal::Return};
    }
};
ScriptVM::ScriptVM() : impl(std::make_shared<Impl>()) {}
void ScriptVM::setGlobal(const std::string &n, ScriptValue v) {
    impl->globals[fold(n)] = std::move(v);
}
ScriptValue ScriptVM::global(const std::string &n) const {
    auto it = impl->globals.find(fold(n));
    if (it == impl->globals.end())
        fail(9, "global not found");
    return it->second;
}
bool ScriptVM::hasGlobal(const std::string &n) const {
    return impl->globals.contains(fold(n));
}
std::unordered_map<std::string, ScriptValue> ScriptVM::globals() const {
    return impl->globals;
}
void ScriptVM::replaceGlobals(std::unordered_map<std::string, ScriptValue> values) {
    impl->globals = std::move(values);
}
void ScriptVM::bind(uint16_t op, Builtin fn) {
    require(!impl->builtins.contains(op), 1, "duplicate builtin binding");
    impl->builtins.emplace(op, std::move(fn));
}
void ScriptVM::unbind(uint16_t op) {
    impl->builtins.erase(op);
}
void ScriptVM::bindRouter(Router router) {
    require(impl->depth == 0, 1, "cannot replace router during active dispatch");
    impl->router = std::move(router);
}
ScriptResult ScriptVM::dispatch(const ScriptProgram &p, const std::string &handler,
                                const std::vector<ScriptValue> &args, const std::string &me,
                                const std::string &target, bool wantsValue) {
    if (impl->depth == 0) {
        impl->remaining = instructionBudget;
        impl->forOpen = impl->whileOpen = 0;
    }
    return impl->invoke(*p.impl, fold(handler), args, me, target, maxCallDepth, true, wantsValue,
                        true);
}
ScriptResult ScriptVM::run(const ScriptProgram &p, const std::string &handler,
                           const std::vector<ScriptValue> &args, const std::string &me,
                           const std::string &target) {
    require(impl->depth == 0, 1, "reentrant native VM dispatch unsupported");
    impl->remaining = instructionBudget;
    impl->forOpen = impl->whileOpen = 0;
    return impl->invoke(*p.impl, fold(handler), args, me, target, maxCallDepth);
}
} // namespace dust
