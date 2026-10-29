#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

// ============================================================================
//  Вспомогательные функции
// ============================================================================
namespace {

Json obj(std::initializer_list<std::pair<const char*, Json>> kv) {
    Json j = Json::object();
    for (const auto& p : kv) j[p.first] = p.second;
    return j;
}

Json arr(std::initializer_list<Json> items) {
    Json a = Json::array();
    for (const auto& x : items) a.push(x);
    return a;
}

Json compound(Json statements) {
    return obj({{"type", "compound-statement"}, {"statements", std::move(statements)}});
}

Json orEmpty(Json s) { return s.isNull() ? compound(Json::array()) : s; }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string nodeType(const Json& n) {
    const Json* t = n.find("type");
    return (t && t->isString()) ? t->str() : std::string();
}

// Имя переменной из узла variable-reference ("$x" -> "x"); для констант — "".
std::string varNameOf(const Json& n) {
    const Json* v = n.find("variable-name");
    if (!v || !v->isString() || v->str().empty() || v->str()[0] != '$') return "";
    return v->str().substr(1);
}

// Может ли выражение стоять слева от присваивания / под ++ и --:
// переменная, элемент массива-переменной, поле объекта, статическое свойство класса.
bool isAssignable(const Json& n) {
    std::string t = nodeType(n);
    if (t == "variable-reference") return !varNameOf(n).empty();  // константы менять нельзя
    if (t == "variable-variable") return true;                     // $$name = ...
    if (t == "index") {
        const Json* a = n.find("array-expression");
        return a && isAssignable(*a);
    }
    if (t == "member-access") {
        const Json* st = n.find("static");
        if (!st) {  // $obj->field: объект должен быть переменной или результатом вызова, а не константой/литералом
            const Json* o = n.find("object");
            if (!o) return false;
            std::string ot = nodeType(*o);
            if (ot == "variable-reference") return !varNameOf(*o).empty();
            return ot == "variable-variable" || ot == "index" || ot == "member-access" || ot == "function-call" ||
                   ot == "dynamic-memory-allocation" || ot == "parentheses";
        }
        const Json* m = n.find("member-name");
        if (n.find("member-expression")) return true;  // Class::$$name
        return m && m->isString() && !m->str().empty() && m->str()[0] == '$';  // Class::$prop, но не Class::CONST
    }
    return false;
}

// Есть ли в цепочке индексаций пустой индекс $a[] (допустим только при записи).
bool hasAppendIndex(const Json& n) {
    if (nodeType(n) != "index") return false;
    if (const Json* idx = n.find("indices"))
        for (const auto& i : idx->items())
            if (i.isNull()) return true;
    const Json* a = n.find("array-expression");
    return a && hasAppendIndex(*a);
}

// Константное выражение: то, что PHP разрешает в значениях по умолчанию, const и static.
bool isConstExpr(const Json& n, bool allowNew) {
    if (n.kind() == Json::Kind::Array) {
        for (const auto& x : n.items())
            if (!isConstExpr(x, allowNew)) return false;
        return true;
    }
    if (n.kind() != Json::Kind::Object) return true;
    std::string t = nodeType(n);
    static const std::set<std::string> deny = {
        "function-call", "variable-variable", "type-cast", "instanceof", "error-suppression", "prefix-increment",
        "postfix-increment", "prefix-decrement", "postfix-decrement", "integer-quotient"};
    if (deny.count(t) || t.rfind("assignment", 0) == 0) return false;
    if (t == "variable-reference") return varNameOf(n).empty();  // константа — можно, переменная — нельзя
    if (t == "member-access") {
        const Json* m = n.find("member-name");
        return n.find("static") && m && m->isString() && (m->str().empty() || m->str()[0] != '$');  // Class::CONST
    }
    if (t == "dynamic-memory-allocation" && !allowNew) return false;
    for (const auto& kv : n.members())
        if (!isConstExpr(kv.second, allowNew)) return false;
    return true;
}

// Зарезервированные слова PHP не могут быть именами констант и функций.
bool isReserved(const std::string& lw) {
    static const std::set<std::string> r = {
        "abstract", "and", "as", "break", "callable", "case", "catch", "class", "clone", "const", "continue",
        "declare", "default", "do", "echo", "else", "elseif", "enddeclare", "endfor", "endforeach", "endif",
        "endswitch", "endwhile", "extends", "final", "finally", "fn", "for", "foreach", "function", "global",
        "goto", "if", "implements", "instanceof", "insteadof", "interface", "namespace", "new", "or", "private",
        "protected", "public", "readonly", "return", "switch", "throw", "trait", "try", "use", "var", "while",
        "xor", "yield", "include", "include_once", "require", "require_once", "print"};
    return r.count(lw) != 0;
}

bool isSuperglobal(const std::string& n) {
    static const std::set<std::string> s = {"GLOBALS", "_SERVER", "_GET",     "_POST", "_FILES", "_COOKIE",
                                            "_SESSION", "_REQUEST", "_ENV", "argv",  "argc"};
    return s.count(n) != 0;
}

// --- правила вывода типов ---
const char* const kIntOrReal = "integer|real";  // union-тип PHP 8: int|float

bool isNumeric(const std::string& t) {
    return t == "integer" || t == "real" || t == "boolean" || t == "null" || t == kIntOrReal;
}

// Значение целочисленного литерала, если узел — литерал.
bool intLiteral(const Json& n, long long& v) {
    const Json* ct = n.find("constant-type");
    const Json* cv = n.find("constant-value");
    if (!ct || !cv || !ct->isString() || ct->str() != "integer" || !cv->isInt()) return false;
    v = cv->asInt();
    return true;
}

std::string arithType(const std::string& a, const std::string& b) {  // + - *
    if (!isNumeric(a) || !isNumeric(b)) return "mixed";
    if (a == "real" || b == "real") return "real";
    if (a == kIntOrReal || b == kIntOrReal) return kIntOrReal;
    return "integer";
}

// В PHP int / int даёт int, если делится нацело, иначе float.
std::string divType(const Json& ln, const std::string& a, const Json& rn, const std::string& b) {
    if (!isNumeric(a) || !isNumeric(b)) return "mixed";
    if (a == "real" || b == "real") return "real";
    long long x = 0, y = 0;
    if (intLiteral(ln, x) && intLiteral(rn, y) && y != 0) return (y == -1 || x % y == 0) ? "integer" : "real";
    return kIntOrReal;
}

// int ** int: int при неотрицательной степени, float при отрицательной.
std::string powType(const std::string& a, const Json& rn, const std::string& b) {
    if (!isNumeric(a) || !isNumeric(b)) return "mixed";
    if (a == "real" || b == "real") return "real";
    long long e = 0;
    if (intLiteral(rn, e) && a != kIntOrReal) return e >= 0 ? "integer" : "real";
    return kIntOrReal;
}

// Совместимо ли значение по умолчанию (литерал) с объявленным типом — проверка, которую PHP делает при компиляции.
bool defaultCompatible(const std::string& declared, const std::string& value, bool implicitNull) {
    std::set<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t bar = declared.find('|', start);
        parts.insert(declared.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    auto has = [&](const char* p) { return parts.count(p) != 0; };
    if (has("mixed")) return true;
    if (value == "null") return implicitNull || has("null");
    if (value == "integer") return has("integer") || has("real");  // int допустим для float
    if (value == "real") return has("real");
    if (value == "string") return has("string");
    if (value == "boolean") return has("boolean");
    if (value == "array") return has("array") || has("iterable");
    return true;  // выражение сложнее литерала — не проверяем
}

std::string phpTypeName(const std::string& t) {
    static const std::map<std::string, std::string> m = {{"integer", "int"}, {"real", "float"}, {"boolean", "bool"}};
    auto it = m.find(t);
    return it != m.end() ? it->second : t;
}

std::string incType(const std::string& t) {  // ++ / --
    if (t == "integer" || t == "real" || t == "string" || t == kIntOrReal) return t;
    if (t.empty() || t == "null") return "integer";
    return "mixed";
}
std::string unify(const std::string& a, const std::string& b) {  // общий тип двух ветвей
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a == b ? a : "mixed";
}

std::string mapPhpType(const std::string& t) {
    static const std::map<std::string, std::string> m = {
        {"int", "integer"},  {"integer", "integer"}, {"float", "real"},  {"double", "real"},
        {"bool", "boolean"}, {"boolean", "boolean"}, {"false", "boolean"}, {"true", "boolean"},
        {"string", "string"}, {"array", "array"},    {"void", "void"},   {"mixed", "mixed"},
        {"null", "null"},    {"callable", "callable"}, {"iterable", "iterable"}, {"object", "object"},
        {"never", "never"}};
    auto it = m.find(lower(t));
    return it != m.end() ? it->second : t;
}

void appendUtf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool isHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// Escape-последовательность в строке "..." начиная с r[i] == '\\'. Возвращает новую позицию.
std::size_t decodeEscape(const std::string& r, std::size_t i, std::string& out) {
    char e = r[i + 1];
    switch (e) {
        case 'n': out += '\n'; return i + 2;
        case 't': out += '\t'; return i + 2;
        case 'r': out += '\r'; return i + 2;
        case 'v': out += '\v'; return i + 2;
        case 'e': out += '\x1b'; return i + 2;
        case 'f': out += '\f'; return i + 2;
        case '\\': out += '\\'; return i + 2;
        case '$': out += '$'; return i + 2;
        case '"': out += '"'; return i + 2;
        case 'u':
            if (i + 2 < r.size() && r[i + 2] == '{') {  // \u{263A}
                std::size_t k = r.find('}', i + 3);
                std::string hex = k == std::string::npos ? "" : r.substr(i + 3, k - i - 3);
                if (hex.empty() || hex.size() > 6 || !std::all_of(hex.begin(), hex.end(), isHex))
                    throw std::runtime_error("invalid UTF-8 codepoint escape sequence");
                unsigned long cp = std::stoul(hex, nullptr, 16);
                if (cp > 0x10FFFF) throw std::runtime_error("invalid UTF-8 codepoint escape sequence: codepoint too large");
                appendUtf8(out, cp);
                return k + 1;
            }
            break;
        case 'x':
            if (i + 2 < r.size() && isHex(r[i + 2])) {
                std::size_t k = i + 2;
                std::string h;
                while (k < r.size() && h.size() < 2 && isHex(r[k])) h += r[k++];
                out += static_cast<char>(std::stoi(h, nullptr, 16));
                return k;
            }
            break;
        default:
            if (e >= '0' && e <= '7') {
                std::size_t k = i + 1;
                int v = 0, cnt = 0;
                while (k < r.size() && cnt < 3 && r[k] >= '0' && r[k] <= '7') {
                    v = v * 8 + (r[k] - '0');
                    ++k;
                    ++cnt;
                }
                out += static_cast<char>(v & 0xFF);
                return k;
            }
    }
    out += '\\';  // неизвестная последовательность остаётся как есть: "\q" -> \q
    out += e;
    return i + 2;
}

// Позиция '}', парной к '{' в r[i].
std::size_t matchBrace(const std::string& r, std::size_t i) {
    int depth = 0;
    for (std::size_t k = i; k < r.size(); ++k) {
        char c = r[k];
        if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return k;
        } else if (c == '\'' || c == '"') {
            for (++k; k < r.size() && r[k] != c; ++k)
                if (r[k] == '\\') ++k;
        }
    }
    return r.size() - 1;
}

// Типы результатов известных встроенных функций PHP.
const std::map<std::string, std::string>& builtinFunctions() {
    static const std::map<std::string, std::string> m = {
        {"strlen", "integer"}, {"count", "integer"}, {"sizeof", "integer"}, {"intval", "integer"},
        {"floatval", "real"}, {"boolval", "boolean"}, {"strval", "string"}, {"abs", "mixed"},
        {"sqrt", "real"}, {"pow", "mixed"}, {"floor", "real"}, {"ceil", "real"}, {"round", "real"},
        {"sin", "real"}, {"cos", "real"}, {"tan", "real"}, {"log", "real"}, {"exp", "real"}, {"pi", "real"},
        {"fmod", "real"}, {"max", "mixed"}, {"min", "mixed"}, {"rand", "integer"}, {"mt_rand", "integer"},
        {"random_int", "integer"}, {"strtoupper", "string"}, {"strtolower", "string"}, {"ucfirst", "string"},
        {"lcfirst", "string"}, {"ucwords", "string"}, {"trim", "string"}, {"ltrim", "string"},
        {"rtrim", "string"}, {"substr", "string"}, {"str_repeat", "string"}, {"str_pad", "string"},
        {"strrev", "string"}, {"sprintf", "string"}, {"number_format", "string"}, {"implode", "string"},
        {"join", "string"}, {"json_encode", "mixed"}, {"md5", "string"}, {"sha1", "string"}, {"chr", "string"},
        {"ord", "integer"}, {"str_replace", "mixed"}, {"strpos", "mixed"}, {"str_contains", "boolean"},
        {"str_starts_with", "boolean"}, {"str_ends_with", "boolean"}, {"explode", "array"},
        {"str_split", "array"}, {"printf", "integer"}, {"print_r", "mixed"}, {"var_dump", "void"},
        {"echo", "void"}, {"print", "integer"}, {"unset", "void"}, {"isset", "boolean"}, {"empty", "boolean"},
        {"is_int", "boolean"}, {"is_integer", "boolean"}, {"is_float", "boolean"}, {"is_string", "boolean"},
        {"is_bool", "boolean"}, {"is_array", "boolean"}, {"is_numeric", "boolean"}, {"is_null", "boolean"},
        {"is_object", "boolean"}, {"in_array", "boolean"}, {"array_key_exists", "boolean"},
        {"function_exists", "boolean"}, {"array_keys", "array"}, {"array_values", "array"},
        {"array_merge", "array"}, {"array_map", "array"}, {"array_filter", "array"}, {"array_slice", "array"},
        {"array_reverse", "array"}, {"array_unique", "array"}, {"range", "array"}, {"array_fill", "array"},
        {"array_combine", "array"}, {"array_flip", "array"}, {"compact", "array"}, {"array_sum", "mixed"},
        {"array_product", "mixed"}, {"array_push", "integer"}, {"array_pop", "mixed"},
        {"array_shift", "mixed"}, {"array_unshift", "integer"}, {"array_search", "mixed"},
        {"sort", "boolean"}, {"rsort", "boolean"}, {"usort", "boolean"}, {"ksort", "boolean"},
        {"asort", "boolean"}, {"arsort", "boolean"}, {"krsort", "boolean"}, {"shuffle", "boolean"},
        {"gettype", "string"}, {"get_class", "string"}, {"time", "integer"}, {"date", "string"},
        {"microtime", "mixed"}, {"readline", "mixed"}, {"fgets", "mixed"}, {"file_get_contents", "mixed"},
        {"define", "boolean"}, {"defined", "boolean"}, {"exit", "void"}, {"die", "void"}, {"clone", "object"}};
    return m;
}

// Типы встроенных констант (имена констант чувствительны к регистру).
const std::map<std::string, std::string>& builtinConstants() {
    static const std::map<std::string, std::string> m = {
        {"PHP_EOL", "string"}, {"PHP_INT_MAX", "integer"}, {"PHP_INT_MIN", "integer"},
        {"PHP_INT_SIZE", "integer"}, {"PHP_FLOAT_EPSILON", "real"}, {"PHP_FLOAT_MAX", "real"},
        {"PHP_FLOAT_MIN", "real"}, {"PHP_VERSION", "string"}, {"PHP_OS", "string"}, {"M_PI", "real"},
        {"M_E", "real"}, {"M_SQRT2", "real"}, {"INF", "real"}, {"NAN", "real"}, {"E_ALL", "integer"},
        {"STDIN", "mixed"}, {"STDOUT", "mixed"}, {"STDERR", "mixed"}, {"SORT_STRING", "integer"},
        {"SORT_NUMERIC", "integer"}, {"__FILE__", "string"}, {"__DIR__", "string"}, {"__METHOD__", "string"},
        {"__NAMESPACE__", "string"}};
    return m;
}

}  // namespace

// ============================================================================
//  Работа с лексемами
// ============================================================================
Parser::Parser(std::vector<Token> tokens, Options options) : toks_(std::move(tokens)), opt_(options) {
    if (toks_.empty() || toks_.back().kind != TokKind::End) toks_.push_back(Token{});
}

const Token& Parser::peekTok(std::size_t k) const { return toks_[std::min(pos_ + k, toks_.size() - 1)]; }

void Parser::advance() {
    if (pos_ + 1 < toks_.size()) ++pos_;
}

bool Parser::isOp(const char* op, std::size_t k) const {
    const Token& t = peekTok(k);
    return t.kind == TokKind::Op && t.text == op;
}

bool Parser::isKw(const char* kw, std::size_t k) const {
    const Token& t = peekTok(k);
    return t.kind == TokKind::Ident && lower(t.text) == kw;  // ключевые слова PHP регистронезависимы
}

bool Parser::acceptOp(const char* op) {
    if (!isOp(op)) return false;
    advance();
    return true;
}

bool Parser::acceptKw(const char* kw) {
    if (!isKw(kw)) return false;
    advance();
    return true;
}

void Parser::expectOp(const char* op) {
    if (!acceptOp(op)) error(std::string("expected '") + op + "'");
}

void Parser::expectKw(const char* kw) {
    if (!acceptKw(kw)) error(std::string("expected '") + kw + "'");
}

void Parser::endStatement() {
    if (acceptOp(";")) return;  // закрывающий ?> лексер тоже превращает в ';'
    error("expected ';'");
}

void Parser::error(const std::string& msg) const {
    const Token& t = cur();
    std::string found;
    switch (t.kind) {
        case TokKind::End: found = "end of file"; break;
        case TokKind::Variable: found = "'$" + t.text + "'"; break;
        case TokKind::String:
        case TokKind::Template: found = "string literal"; break;
        case TokKind::InlineHtml: found = "inline HTML"; break;
        default: found = "'" + t.text + "'";
    }
    throw std::runtime_error("line " + std::to_string(t.line) + ":" + std::to_string(t.col) + ": " + msg +
                             ", found " + found);
}

// ============================================================================
//  Типы и таблица символов
// ============================================================================
Json Parser::typeNode(const std::string& t0) const {
    std::string t = t0.empty() ? "mixed" : t0;
    if (opt_.shortTypes) return Json(t);
    static const std::set<std::string> builtin = {"integer", "real",  "string",   "boolean",  "null",  "array",
                                                  "mixed",   "void",  "object",   "callable", "never", "iterable"};
    if (builtin.count(t) || t.find('|') != std::string::npos)
        return obj({{"type", "builtin-type"}, {"type-value", t}});
    return obj({{"type", "named-type"}, {"name", t}});  // расширение схемы: ссылка на класс по имени
}

// Тип в объявлении: int, ?string, int|float, \Foo, self ...
std::string Parser::parseTypeHint() {
    std::vector<std::string> parts;
    bool nullable = acceptOp("?");
    while (true) {
        if (acceptOp("(")) {  // DNF-тип (A&B)|null — упрощаем до object
            while (!acceptOp(")")) {
                if (cur().kind == TokKind::End) error("unterminated type");
                advance();
            }
            parts.push_back("object");
        } else {
            if (cur().kind != TokKind::Ident) error("type expected");
            std::string t = cur().text;
            advance();
            std::string lt = lower(t);
            if ((lt == "self" || lt == "static") && !currentClass_.empty()) t = currentClass_;
            parts.push_back(mapPhpType(t));
        }
        if (acceptOp("|")) continue;
        if (isOp("&") && peekTok(1).kind == TokKind::Ident) {  // пересечение A&B
            advance();
            continue;
        }
        break;
    }
    for (const char* standalone : {"void", "never", "mixed"})
        if (std::find(parts.begin(), parts.end(), standalone) != parts.end() && (nullable || parts.size() > 1))
            error(std::string(standalone) + " can only be used as a standalone type");
    if (nullable) parts.push_back("null");
    std::string r;
    for (const auto& p : parts) r += (r.empty() ? "" : "|") + p;
    return r;
}

std::string Parser::resolveClass(const std::string& name) const {
    std::string ln = lower(name);
    if (ln == "self" || ln == "static") return currentClass_.empty() ? name : currentClass_;
    if (ln == "parent") {
        auto it = classes_.find(lower(currentClass_));
        return (it != classes_.end() && !it->second.parent.empty()) ? it->second.parent : name;
    }
    return name;
}

// which: 0 — свойство, 1 — метод, 2 — константа класса. Поиск идёт вверх по цепочке наследования.
std::string Parser::lookupMember(std::string cls, const std::string& name, int which) const {
    for (int guard = 0; guard < 32 && !cls.empty(); ++guard) {
        auto it = classes_.find(lower(cls));
        if (it == classes_.end()) break;
        const auto& m = which == 0 ? it->second.props : which == 1 ? it->second.methods : it->second.consts;
        auto f = m.find(which == 1 ? lower(name) : name);
        if (f != m.end()) return f->second;
        cls = it->second.parent;
    }
    return "mixed";
}

static bool isThis(const Json& n) { return nodeType(n) == "variable-reference" && varNameOf(n) == "this"; }

std::string Parser::functionType(const std::string& lname0) const {
    std::string lname = lname0;
    std::size_t slash = lname.rfind('\\');
    if (slash != std::string::npos) lname = lname.substr(slash + 1);
    auto it = funcTypes_.find(lname);
    if (it != funcTypes_.end()) return it->second;
    auto b = builtinFunctions().find(lname);
    return b != builtinFunctions().end() ? b->second : "mixed";
}

Parser::Expr Parser::varRef(const std::string& name) {
    std::string type;
    if (name == "this" && !currentClass_.empty()) {
        type = currentClass_;
    } else if (isSuperglobal(name)) {
        type = name == "argc" ? "integer" : "array";
    } else {
        Scope& s = scope();
        auto it = s.vars.find(name);
        if (it == s.vars.end()) {  // первое появление — неявное объявление
            s.vars[name] = VarInfo{};
            s.order.push_back(name);
        } else {
            type = it->second.type;
        }
    }
    if (type.empty()) type = "mixed";
    return {obj({{"type", "variable-reference"},
                 {"variable-name", "$" + name},
                 {"variable-type", typeNode(type)},
                 {"variable-usage", "rvalue"}}),
            type};
}

// Константа представляется как variable-reference на константу из constant-section.
Parser::Expr Parser::constRef(const std::string& name) {
    std::string type = "mixed";
    auto it = constTypes_.find(name);
    if (it != constTypes_.end()) type = it->second;
    else {
        auto b = builtinConstants().find(name);
        if (b != builtinConstants().end()) type = b->second;
    }
    return {obj({{"type", "variable-reference"},
                 {"variable-name", name},
                 {"variable-type", typeNode(type)},
                 {"variable-usage", "rvalue"}}),
            type};
}

void Parser::setVarType(const std::string& name, const std::string& t) {
    if (name.empty() || name == "this" || isSuperglobal(name)) return;
    Scope& s = scope();
    auto it = s.vars.find(name);
    if (it == s.vars.end()) {
        s.vars[name] = VarInfo{};
        s.order.push_back(name);
        it = s.vars.find(name);
    }
    it->second.type = t;
    it->second.declType = unify(it->second.declType, t);
}

// Помечает выражение как изменяемое (стоящее слева от присваивания, под ++ и т. п.).
void Parser::markLvalue(Json& n) {
    std::string t = nodeType(n);
    if (t == "variable-reference") n["variable-usage"] = "lvalue";
    else if (t == "index") {
        if (Json* a = n.find("array-expression")) markLvalue(*a);
    }
}

// После присваивания обновляет тип переменной в таблице символов и в самом узле.
void Parser::updateTargetType(Json& n, const std::string& t) {
    std::string nt = nodeType(n);
    if (nt == "variable-reference") {
        std::string v = varNameOf(n);
        if (v.empty() || v == "this" || isSuperglobal(v)) return;
        setVarType(v, t);
        n["variable-type"] = typeNode(t);
    } else if (nt == "index") {
        Json* root = n.find("array-expression");
        while (root && nodeType(*root) == "index") root = root->find("array-expression");
        if (!root || nodeType(*root) != "variable-reference") return;
        std::string v = varNameOf(*root);
        if (v.empty() || v == "this" || isSuperglobal(v)) return;
        auto it = scope().vars.find(v);
        std::string curT = it != scope().vars.end() ? it->second.type : "";
        if (curT.empty() || curT == "null") {  // $a[] = 1 на неинициализированной переменной создаёт массив
            setVarType(v, "array");
            (*root)["variable-type"] = typeNode("array");
        }
    }
}

// Предварительный проход: собираем объявленные типы результатов функций и методов,
// чтобы вызовы до объявления (в PHP функции "всплывают") тоже получили тип.
void Parser::prescan() {
    std::vector<std::pair<std::string, int>> classStack;
    std::string pendingClass;
    int depth = 0;
    for (std::size_t k = 0; k < toks_.size(); ++k) {
        const Token& t = toks_[k];
        if (t.kind == TokKind::Op) {
            if (t.text == "{") {
                ++depth;
                if (!pendingClass.empty()) {
                    classStack.push_back({pendingClass, depth});
                    pendingClass.clear();
                }
            } else if (t.text == "}") {
                if (!classStack.empty() && classStack.back().second == depth) classStack.pop_back();
                --depth;
            }
            continue;
        }
        if (t.kind != TokKind::Ident) continue;
        std::string lw = lower(t.text);
        bool afterColons = k > 0 && toks_[k - 1].kind == TokKind::Op && toks_[k - 1].text == "::";
        if ((lw == "class" || lw == "interface") && !afterColons && k + 1 < toks_.size() &&
            toks_[k + 1].kind == TokKind::Ident) {
            pendingClass = toks_[k + 1].text;
            continue;
        }
        if (lw != "function") continue;
        std::size_t j = k + 1;
        if (j < toks_.size() && toks_[j].kind == TokKind::Op && toks_[j].text == "&") ++j;
        if (j >= toks_.size() || toks_[j].kind != TokKind::Ident) continue;
        std::string name = lower(toks_[j].text);
        ++j;
        if (j >= toks_.size() || toks_[j].kind != TokKind::Op || toks_[j].text != "(") continue;
        int pd = 0;
        for (; j < toks_.size(); ++j) {
            if (toks_[j].kind != TokKind::Op) continue;
            if (toks_[j].text == "(") ++pd;
            else if (toks_[j].text == ")" && --pd == 0) break;
        }
        if (j + 1 >= toks_.size() || toks_[j + 1].kind != TokKind::Op || toks_[j + 1].text != ":") continue;
        std::size_t savedPos = pos_;
        std::string savedClass = currentClass_;
        currentClass_ = classStack.empty() ? "" : classStack.back().first;
        pos_ = j + 2;
        try {
            std::string rt = parseTypeHint();
            if (classStack.empty()) funcTypes_[name] = rt;
            else classes_[lower(classStack.back().first)].methods[name] = rt;
        } catch (...) {
            // ошибка будет выдана при основном разборе
        }
        pos_ = savedPos;
        currentClass_ = savedClass;
    }
}

// ============================================================================
//  Построители узлов
// ============================================================================
Json Parser::declarator(const std::string& name, const std::string& type, Json init, bool ref) const {
    return obj({{"type", "declarator"},
                {"variable-type", typeNode(type)},
                {"variable-name", name},
                {"reference", ref},
                {"constructor-name", nullptr},
                {"constructor-arguments", std::move(init)}});
}

Json Parser::literal(const char* ctype, Json value) const {
    return obj({{"type", "literal-constant"}, {"constant-type", ctype}, {"constant-value", std::move(value)}});
}

Parser::Expr Parser::callNode(const std::string& name, const std::vector<Expr>& args, const std::string& type) const {
    Json a = Json::array();
    for (const auto& e : args) a.push(e.node);
    return {obj({{"type", "function-call"}, {"function-name", name}, {"result-type", typeNode(type)}, {"arguments", a}}),
            type};
}

Json Parser::finishSubroutine(const std::string& name, const std::string& rtype, Json args, Json body) {
    Scope& s = scope();
    // goto может ссылаться на метку, объявленную ниже, поэтому проверяем в конце подпрограммы
    for (const auto& g : s.gotos)
        if (!s.labels.count(g.first))
            throw std::runtime_error("line " + std::to_string(g.second.line) + ":" + std::to_string(g.second.col) +
                                     ": 'goto' to undefined label '" + g.first + "'");
    Json vars = Json::array();
    for (const auto& d : s.explicitVars.items()) vars.push(d);
    Json decls = Json::array();
    for (const auto& v : s.order) {
        const VarInfo& info = s.vars[v];
        if (info.explicitDecl) continue;
        decls.push(declarator("$" + v, info.declType.empty() ? "mixed" : info.declType, Json::array()));
    }
    if (decls.size())
        vars.push(obj({{"type", "variables-declaration"},
                       {"global", s.isMain},
                       {"static", false},
                       {"const", false},
                       {"declarators", decls}}));
    // Пометка последнего оператора return
    if (Json* st = body.find("statements")) {
        if (st->size() && nodeType(st->back()) == "return-statement") st->back()["the-last-statement"] = true;
    }
    return obj({{"type", "subroutine-declaration"},
                {"subroutine-name", name},
                {"result-type", typeNode(rtype)},
                {"arguments-declarators", std::move(args)},
                {"constant-section", s.constants},
                {"type-section", s.types},
                {"variables-section", vars},
                {"subroutines-section", s.subroutines},
                {"subroutine-body", std::move(body)}});
}

void Parser::requireConstExpr(const Expr& e, const Token& at, bool allowNew) const {
    if (!isConstExpr(e.node, allowNew))
        throw std::runtime_error("line " + std::to_string(at.line) + ":" + std::to_string(at.col) +
                                 ": constant expression contains invalid operations");
}

// ============================================================================
//  Программа
// ============================================================================
Json Parser::parseProgram() {
    prescan();
    Scope mainScope;
    mainScope.name = "main";
    mainScope.isMain = true;
    scopes_.push_back(std::move(mainScope));
    Json st = Json::array();
    while (cur().kind != TokKind::End) {
        Json s = parseStatement();
        if (!s.isNull()) st.push(s);
    }
    // Весь скрипт оформляется как подпрограмма main: глобальные переменные, функции и классы
    // попадают в её секции, а операторы верхнего уровня — в её тело.
    Json result = finishSubroutine("main", "void", Json::array(), compound(st));
    scopes_.pop_back();
    return result;
}

// ============================================================================
//  Выражения
// ============================================================================
// Уровни приоритета (от низшего к высшему):
//   or < xor < and < присваивание < ?: < ?? < || < && < | < ^ < & < == != === <=> < < <= > >=
//   < . < << >> < + - < * / % < instanceof < ! ~ - + ++ -- (cast) @ < ** < [] -> :: ()
Parser::Expr Parser::parseExpr() {
    Expr l = parseKeywordXor();
    while (acceptKw("or")) {
        Expr r = parseKeywordXor();
        l = {obj({{"type", "logical-or"}, {"result-type", "boolean"}, {"left", l.node}, {"right", r.node}}), "boolean"};
    }
    return l;
}

Parser::Expr Parser::parseKeywordXor() {
    Expr l = parseKeywordAnd();
    while (acceptKw("xor")) {
        Expr r = parseKeywordAnd();
        l = {obj({{"type", "logical-xor"}, {"result-type", "boolean"}, {"left", l.node}, {"right", r.node}}), "boolean"};
    }
    return l;
}

Parser::Expr Parser::parseKeywordAnd() {
    Expr l = parseTernary();
    while (acceptKw("and")) {
        Expr r = parseTernary();
        l = {obj({{"type", "logical-and"}, {"result-type", "boolean"}, {"left", l.node}, {"right", r.node}}), "boolean"};
    }
    return l;
}

Parser::Expr Parser::parseTernary() {
    Expr c = parseBinary(1);
    bool first = true, prevShort = false;
    while (isOp("?")) {
        advance();
        bool shortForm = isOp(":");
        // PHP 8: a ? b : c ? d : e без скобок — ошибка; допустима только цепочка a ?: b ?: c
        if (!first && !(prevShort && shortForm)) error("unparenthesized nested ternary operator is not allowed");
        first = false;
        prevShort = shortForm;
        Expr t;
        if (shortForm) t = c;  // короткая форма  a ?: b  ==  a ? a : b
        else t = parseTernary();
        expectOp(":");
        Expr f = parseBinary(1);
        std::string rt = unify(t.type, f.type);
        c = {obj({{"type", "conditional-expression"},
                  {"result-type", typeNode(rt)},
                  {"condition", c.node},
                  {"if-true", t.node},
                  {"if-false", f.node}}),
             rt};
    }
    return c;
}

// Бинарные операции — метод "восхождения по приоритетам" (precedence climbing).
Parser::Expr Parser::parseBinary(int minPrec) {
    static const std::map<std::string, int> table = {
        {"??", 1}, {"||", 2}, {"&&", 3}, {"|", 4}, {"^", 5}, {"&", 6},
        {"==", 7}, {"!=", 7}, {"===", 7}, {"!==", 7}, {"<=>", 7},
        {"<", 8}, {"<=", 8}, {">", 8}, {">=", 8},
        {".", 9}, {"<<", 10}, {">>", 10}, {"+", 11}, {"-", 11}, {"*", 12}, {"/", 12}, {"%", 12}};
    Expr left = parseUnary();
    int lastPrec = -1;
    while (true) {
        std::string op;
        int prec = 0;
        if (isKw("instanceof")) {
            op = "instanceof";
            prec = 13;
        } else if (cur().kind == TokKind::Op) {
            auto it = table.find(cur().text);
            if (it == table.end()) break;
            op = it->first;
            prec = it->second;
        } else {
            break;
        }
        if (prec < minPrec) break;
        // == != === <=> и < <= > >= неассоциативны: a < b < c — ошибка в PHP
        if ((prec == 7 || prec == 8) && lastPrec == prec) error("comparison operators cannot be chained");
        lastPrec = prec;
        advance();
        if (op == "instanceof") {
            Json right = parseClassRef();
            left = {obj({{"type", "instanceof"}, {"result-type", "boolean"}, {"left", left.node}, {"right", right}}),
                    "boolean"};
            continue;
        }
        bool rightAssoc = (op == "??");
        Expr rhs = parseBinary(rightAssoc ? prec : prec + 1);
        left = makeBinary(op, left, rhs);
    }
    return left;
}

// Правая часть instanceof: имя класса или выражение.
Json Parser::parseClassRef() {
    if (cur().kind == TokKind::Ident) {
        Json r = typeNode(resolveClass(cur().text));
        advance();
        return r;
    }
    return parseUnary().node;
}

Parser::Expr Parser::makeBinary(const std::string& op, const Expr& l, const Expr& r) {
    // Узлы с "result-type": тип  — тип выводится и печатается узлом типа;
    // узлы, где в схеме тип задан конкретной строкой ("boolean", "int", "string"), — печатаем её буквально.
    auto arith = [&](const char* name, const std::string& t) {
        return Expr{obj({{"type", name}, {"result-type", typeNode(t)}, {"left", l.node}, {"right", r.node}}), t};
    };
    auto cmp = [&](const char* name, bool strict) {
        Json n = obj({{"type", name}, {"result-type", "boolean"}, {"left", l.node}, {"right", r.node}});
        if (strict) n["strict"] = true;  // === и !== (расширение схемы)
        return Expr{n, "boolean"};
    };
    auto bit = [&](const char* name) {
        return Expr{obj({{"type", name}, {"result-type", "int"}, {"left", l.node}, {"right", r.node}}), "integer"};
    };
    auto shift = [&](const char* name) {
        return Expr{obj({{"type", name}, {"result-type", "int"}, {"amount", r.node}, {"argument", l.node}}), "integer"};
    };
    if (op == "+") return arith("sum", (l.type == "array" && r.type == "array") ? "array" : arithType(l.type, r.type));
    if (op == "-") return arith("difference", arithType(l.type, r.type));
    if (op == "*") return arith("product", arithType(l.type, r.type));
    if (op == "/") return arith("quotient", divType(l.node, l.type, r.node, r.type));
    if (op == "%") return arith("modulo", "integer");
    if (op == ".")
        return {obj({{"type", "string-concatenation"}, {"result-type", "string"}, {"left", l.node}, {"right", r.node}}),
                "string"};
    if (op == "==") return cmp("equals", false);
    if (op == "===") return cmp("equals", true);
    if (op == "!=") return cmp("not-equal", false);
    if (op == "!==") return cmp("not-equal", true);
    if (op == "<") return cmp("less", false);
    if (op == "<=") return cmp("less-or-equal", false);
    if (op == ">") return cmp("greater", false);
    if (op == ">=") return cmp("greater-or-equal", false);
    if (op == "<=>") return arith("three-way-comparison", "integer");
    if (op == "&&") return cmp("logical-and", false);
    if (op == "||") return cmp("logical-or", false);
    if (op == "&") return bit("bitwise-and");
    if (op == "|") return bit("bitwise-or");
    if (op == "^") return bit("bitwise-xor");
    if (op == "<<") return shift("bitwise-shift-left");
    if (op == ">>") return shift("arithmetic-bitwise-shift-right");  // в PHP >> — арифметический сдвиг
    if (op == "??") return arith("null-coalescing", unify(l.type == "null" ? "" : l.type, r.type));
    throw std::logic_error("unknown binary operator " + op);
}

Parser::Expr Parser::parseUnary() {
    DepthGuard guard(*this);
    auto unary = [&](const char* name, Json rt, const Expr& a, const std::string& t) {
        return Expr{obj({{"type", name}, {"result-type", std::move(rt)}, {"argument", a.node}}), t};
    };
    if (acceptOp("!")) {
        // instanceof связывает сильнее, чем !:  !$a instanceof B  ==  !($a instanceof B)
        Expr a = parseUnary();
        while (isKw("instanceof")) {
            advance();
            Json right = parseClassRef();
            a = {obj({{"type", "instanceof"}, {"result-type", "boolean"}, {"left", a.node}, {"right", right}}), "boolean"};
        }
        return unary("logical-not", "boolean", a, "boolean");
    }
    if (acceptOp("~")) {
        Expr a = parseUnary();
        return unary("bitwise-not", "int", a, "integer");
    }
    if (isOp("-") || isOp("+")) {
        bool minus = cur().text == "-";
        advance();
        Expr a = parseUnary();
        if (nodeType(a.node) == "literal-constant") {  // свёртка: -5 — это константа -5
            Json* v = a.node.find("constant-value");
            if (v && v->isInt()) {
                if (minus) *v = Json(-v->asInt());
                return a;
            }
            if (v && v->isReal()) {
                if (minus) *v = Json(-v->asReal());
                return a;
            }
        }
        std::string t = arithType(a.type, "integer");
        return unary(minus ? "unary-minus" : "unary-plus", typeNode(t), a, t);
    }
    if (isOp("++") || isOp("--")) {
        bool inc = cur().text == "++";
        advance();
        Expr a;
        {
            ScopedSet<bool> w(writeContext_, true);  // ++$a[] допустимо
            a = parseUnary();
        }
        if (!isAssignable(a.node)) error("variable expected after ++/--");
        std::string t = incType(a.type);
        markLvalue(a.node);
        updateTargetType(a.node, t);
        return unary(inc ? "prefix-increment" : "prefix-decrement", typeNode(t), a, t);
    }
    if (acceptOp("@")) {  // подавление ошибок
        Expr a = parseUnary();
        return unary("error-suppression", typeNode(a.type), a, a.type);
    }
    if (isOp("(") && peekTok(1).kind == TokKind::Ident && isOp(")", 2)) {  // (int)$x
        static const std::map<std::string, std::string> casts = {
            {"int", "integer"}, {"integer", "integer"}, {"bool", "boolean"}, {"boolean", "boolean"},
            {"float", "real"},  {"double", "real"},     {"real", "real"},    {"string", "string"},
            {"binary", "string"}, {"array", "array"},   {"object", "object"}, {"unset", "null"}};
        auto it = casts.find(lower(peekTok(1).text));
        if (it != casts.end()) {
            advance();
            advance();
            advance();
            Expr a = parseUnary();
            return unary("type-cast", typeNode(it->second), a, it->second);
        }
    }
    return parsePow();
}

Parser::Expr Parser::parsePow() {
    Expr base = parsePostfix();
    if (acceptOp("**")) {
        Expr e = parseUnary();  // правоассоциативно: 2 ** 3 ** 2 == 2 ** (3 ** 2); допускает 2 ** -1
        std::string t = powType(base.type, e.node, e.type);
        return {obj({{"type", "power"}, {"result-type", typeNode(t)}, {"left", base.node}, {"right", e.node}}), t};
    }
    return base;
}

Parser::Expr Parser::parsePostfix(bool allowAssign) {
    Expr e = parsePrimary();
    while (true) {
        if (isOp("[")) {  // индексация; $a[1][2] сворачивается в один узел с indices [1, 2]
            advance();
            Json idx;
            if (!isOp("]")) idx = parseExpr().node;
            expectOp("]");
            std::string et = e.type == "string" ? "string" : "mixed";
            if (nodeType(e.node) == "index") {
                e.node["indices"].push(idx);
                e.node["result-type"] = typeNode(et);
            } else {
                e.node = obj({{"type", "index"},
                              {"array-expression", e.node},
                              {"result-type", typeNode(et)},
                              {"indices", arr({idx})}});
            }
            e.type = et;
        } else if (isOp("->") || isOp("?->")) {
            if (nodeType(e.node) == "literal-constant" || nodeType(e.node) == "array-literal") error("unexpected '->'");
            bool nullsafe = cur().text == "?->";
            advance();
            std::string m;
            if (cur().kind == TokKind::Ident) m = cur().text;
            else if (cur().kind == TokKind::Variable) m = "$" + cur().text;
            else error("member name expected");
            advance();
            if (isOp("(")) {  // вызов метода
                std::vector<Expr> args = parseArgs();
                std::string t = lookupMember(e.type, m, 1);
                Expr c = callNode(m, args, t);
                c.node["object"] = e.node;
                if (nullsafe) c.node["null-safe"] = true;
                e = c;
            } else {  // обращение к полю (расширение схемы)
                std::string t = lookupMember(e.type, m, 0);
                Json n = obj({{"type", "member-access"}, {"result-type", typeNode(t)}, {"object", e.node}, {"member-name", m}});
                if (nullsafe) n["null-safe"] = true;
                e = {n, t};
            }
        } else if (isOp("(") && (nodeType(e.node) == "variable-reference" || nodeType(e.node) == "index" ||
                                 nodeType(e.node) == "member-access" || nodeType(e.node) == "parentheses")) {
            // вызов через выражение: $f(1)
            std::vector<Expr> args = parseArgs();
            Expr c = callNode("", args, "mixed");
            c.node["function-name"] = nullptr;
            c.node["function-expression"] = e.node;
            e = c;
        } else if (isOp("::")) {  // $obj::CONST, $obj::method(), 'Foo'::BAR
            std::string bt = nodeType(e.node);
            const Json* ct = e.node.find("constant-type");
            bool ok = (bt == "variable-reference" && !varNameOf(e.node).empty()) || bt == "index" ||
                      bt == "member-access" || bt == "function-call" || bt == "variable-variable" ||
                      bt == "parentheses" || (bt == "literal-constant" && ct && ct->isString() && ct->str() == "string");
            if (!ok) error("unexpected '::'");
            Json base = e.node;
            e = parseStaticAccess(e.type, &base);
        } else if (isOp("++") || isOp("--")) {
            if (!isAssignable(e.node)) break;
            bool inc = cur().text == "++";
            advance();
            std::string t = incType(e.type);
            std::string before = e.type;  // постфиксная форма возвращает старое значение
            markLvalue(e.node);
            updateTargetType(e.node, t);
            e = {obj({{"type", inc ? "postfix-increment" : "postfix-decrement"},
                      {"result-type", typeNode(before == "mixed" && t != "mixed" ? t : before)},
                      {"argument", e.node}}),
                 before};
        } else {
            break;
        }
    }
    // Присваивание: его левая часть — "переменная" (variable, $a[..], $o->x), правая — выражение.
    static const std::set<std::string> assignOps = {"=",  "+=", "-=", "*=", "/=", "%=",  "**=",
                                                    ".=", "?\?=", "&=", "|=", "^=", "<<=", ">>="};
    if (allowAssign && cur().kind == TokKind::Op && assignOps.count(cur().text)) {
        if (isAssignable(e.node)) return parseAssignment(e);
        std::string nt = nodeType(e.node);
        if (nt == "variable-reference") error("cannot assign to a constant");
        if (nt == "member-access" || nt == "index") error("cannot write to a temporary expression");
    }
    if (allowAssign && !writeContext_ && hasAppendIndex(e.node)) error("cannot use [] for reading");
    return e;
}

Parser::Expr Parser::parseAssignment(Expr target) {
    std::string op = cur().text;
    if (op == "=" && isThis(target.node)) error("cannot re-assign $this");
    advance();
    bool byRef = false;
    if (op == "=" && acceptOp("&")) byRef = true;  // $a = &$b
    Token rhsTok = cur();
    Expr rhs;
    {
        ScopedSet<bool> w(writeContext_, byRef);  // $r = &$a[] допустимо
        // справа от =& — переменная: $x =& $i + 1  ==  ($x =& $i) + 1
        rhs = byRef ? parsePostfix(/*allowAssign=*/false) : parseTernary();
    }
    if (byRef) {  // ссылку можно взять только на переменную (или результат вызова / new)
        std::string rt = nodeType(rhs.node);
        if (!isAssignable(rhs.node) && rt != "function-call" && rt != "dynamic-memory-allocation" && rt != "integer-quotient")
            throw std::runtime_error("line " + std::to_string(rhsTok.line) + ":" + std::to_string(rhsTok.col) +
                                     ": only variables can be assigned by reference");
    }
    const std::string& old = target.type;
    std::string t;
    if (op == "=") t = rhs.type;
    else if (op == "+=" || op == "-=" || op == "*=") t = arithType(old, rhs.type);
    else if (op == "/=") t = divType(target.node, old, rhs.node, rhs.type);
    else if (op == "%=") t = "integer";
    else if (op == "**=") t = powType(old, rhs.node, rhs.type);
    else if (op == ".=") t = "string";
    else if (op == "?\?=") t = unify(old == "null" ? "" : old, rhs.type);
    else t = "integer";  // побитовые
    markLvalue(target.node);
    updateTargetType(target.node, t);

    static const std::map<std::string, std::string> names = {
        {"=", "assignment"}, {"+=", "assignment-sum"}, {"-=", "assignment-difference"},
        {"*=", "assignment-product"}, {"/=", "assignment-quotient"}, {"%=", "assignment-modulo"},
        {"**=", "assignment-power"}, {".=", "assignment-string-concatenation"},
        {"?\?=", "assignment-null-coalescing"}, {"&=", "assignment-bitwise-and"}, {"|=", "assignment-bitwise-or"},
        {"^=", "assignment-bitwise-xor"}, {"<<=", "assignment-bitwise-shift-left"},
        {">>=", "assignment-arithmetic-bitwise-shift-right"}};
    const std::string& name = names.at(op);
    Json node;
    if (op == "&=" || op == "|=" || op == "^=") {
        // Схема описывает побитовые присваивания полями result-type/left/right
        node = obj({{"type", name}, {"result-type", "int"}, {"left", target.node}, {"right", rhs.node}});
    } else if (op == "<<=" || op == ">>=") {
        // ...а сдвиги с присваиванием — полями amount/argument
        node = obj({{"type", name}, {"result-type", "int"}, {"amount", rhs.node}, {"argument", target.node}});
    } else {
        node = obj({{"type", name}, {"assignment-type", typeNode(t)}, {"variable", target.node}, {"expression", rhs.node}});
        if (byRef) node["by-reference"] = true;
    }
    return {node, t};
}

Parser::Expr Parser::parsePrimary() {
    Token t = cur();
    switch (t.kind) {
        case TokKind::Variable:
            advance();
            return varRef(t.text);
        case TokKind::Int:
            advance();
            return {literal("integer", Json(t.ival)), "integer"};
        case TokKind::Real:
            advance();
            return {literal("real", Json(t.rval)), "real"};
        case TokKind::String:
            advance();
            return {literal("string", t.text), "string"};
        case TokKind::Template:
            advance();
            try {
                return parseTemplate(t.text);
            } catch (const std::runtime_error& e) {
                std::string m = e.what();
                if (m.rfind("line ", 0) == 0) throw;
                throw std::runtime_error("line " + std::to_string(t.line) + ":" + std::to_string(t.col) + ": " + m);
            }
        case TokKind::Op:
            if (t.text == "(") {
                advance();
                Expr e = parseExpr();
                expectOp(")");
                return {obj({{"type", "parentheses"}, {"result-type", typeNode(e.type)}, {"argument", e.node}}), e.type};
            }
            if (t.text == "[") {
                advance();
                return parseArrayLiteral("]");
            }
            if (t.text == "$") {  // переменная переменная: $$name или ${expr}
                advance();
                Expr inner;
                if (cur().kind == TokKind::Variable || isOp("$")) {
                    inner = parsePrimary();
                } else if (acceptOp("{")) {
                    inner = parseExpr();
                    expectOp("}");
                } else {
                    error("variable name expected after '$'");
                }
                return {obj({{"type", "variable-variable"}, {"result-type", typeNode("mixed")}, {"argument", inner.node}}),
                        "mixed"};
            }
            break;
        case TokKind::Ident: {
            std::string name = t.text;
            std::string lw = lower(name);
            if (lw == "true" || lw == "false") {
                advance();
                return {literal("boolean", Json(lw == "true")), "boolean"};
            }
            if (lw == "null") {
                advance();
                return {literal("null", Json()), "null"};
            }
            if (lw == "new") return parseNew();
            if (lw == "clone") {
                advance();
                Expr a = parseUnary();
                return callNode("clone", {a}, a.type);
            }
            if (lw == "array" && isOp("(", 1)) {
                advance();
                advance();
                return parseArrayLiteral(")");
            }
            if (lw == "print") {
                advance();
                Expr a = parseTernary();
                return callNode("print", {a}, "integer");
            }
            if (lw == "exit" || lw == "die") {
                advance();
                std::vector<Expr> args;
                if (isOp("(")) args = parseArgs();
                return callNode(lw, args, "void");
            }
            if (lw == "include" || lw == "include_once" || lw == "require" || lw == "require_once") {
                advance();
                Expr a = parseTernary();
                return callNode(lw, {a}, "mixed");
            }
            if (lw == "function" || lw == "fn") error("anonymous functions are not supported");
            if (lw == "match") error("match expressions are not supported");
            if (lw == "list") error("list() destructuring is not supported");
            if (lw == "__line__") {
                advance();
                return {literal("integer", Json(static_cast<long long>(t.line))), "integer"};
            }
            if (lw == "__function__") {
                advance();
                return {literal("string", scope().isMain ? "" : scope().name), "string"};
            }
            if (lw == "__class__") {
                advance();
                return {literal("string", currentClass_), "string"};
            }
            advance();
            if (isOp("::")) return parseStaticAccess(name);
            static const std::set<std::string> classWords = {"static", "self", "parent", "array"};
            static const std::set<std::string> callOnly = {"isset", "empty", "unset", "eval"};
            if (isReserved(lw) || classWords.count(lw) || (callOnly.count(lw) && !isOp("("))) {
                --pos_;  // чтобы сообщение указывало на само слово
                error("unexpected keyword");
            }
            if (isOp("(")) return makeCall(name);
            return constRef(name);
        }
        default:
            break;
    }
    error("expression expected");
}

Parser::Expr Parser::makeCall(const std::string& name) {
    std::vector<Expr> args = parseArgs();
    std::string lw = lower(name);
    if (lw == "unset")
        for (const auto& a : args)
            if (isThis(a.node)) error("cannot unset $this");
    if (lw == "intdiv" && args.size() == 2) {  // intdiv(a, b) — это и есть "неполное частное"
        return {obj({{"type", "integer-quotient"},
                     {"result-type", typeNode("integer")},
                     {"left", args[0].node},
                     {"right", args[1].node}}),
                "integer"};
    }
    return callNode(name, args, functionType(lw));
}

std::vector<Parser::Expr> Parser::parseArgs() {
    expectOp("(");
    std::vector<Expr> args;
    while (!isOp(")")) {
        if (acceptOp("...")) {
            if (isOp(")")) error("first-class callable syntax is not supported");
            Expr e = parseExpr();
            args.push_back({obj({{"type", "spread"}, {"argument", e.node}}), "array"});
        } else if (cur().kind == TokKind::Ident && isOp(":", 1)) {  // именованный аргумент f(name: 1)
            std::string n = cur().text;
            advance();
            advance();
            Expr e = parseExpr();
            args.push_back({obj({{"type", "named-argument"}, {"name", n}, {"value", e.node}}), e.type});
        } else {
            args.push_back(parseExpr());
        }
        if (!acceptOp(",")) break;
    }
    expectOp(")");
    return args;
}

Parser::Expr Parser::parseArrayLiteral(const char* close) {
    Json elems = Json::array();
    while (!isOp(close)) {
        if (acceptOp("...")) {
            Expr e = parseExpr();
            elems.push(obj({{"type", "array-element"}, {"key", nullptr}, {"value", obj({{"type", "spread"}, {"argument", e.node}})}}));
        } else {
            bool byRef = acceptOp("&");
            Expr v = parseExpr();
            Json key;
            if (acceptOp("=>")) {
                if (byRef) error("'&' must precede the value, not the key");
                key = v.node;
                byRef = acceptOp("&");
                v = parseExpr();
            }
            if (byRef && !isAssignable(v.node) && nodeType(v.node) != "function-call")
                error("only variables can be taken by reference");
            Json el = obj({{"type", "array-element"}, {"key", key}, {"value", v.node}});
            if (byRef) el["by-reference"] = true;
            elems.push(el);
        }
        if (!acceptOp(",")) break;
    }
    expectOp(close);
    return {obj({{"type", "array-literal"}, {"result-type", typeNode("array")}, {"elements", elems}}), "array"};
}

// new Class(args) -> выделение динамической памяти под скаляр с вызовом конструктора
Parser::Expr Parser::parseNew() {
    advance();  // new
    std::string cls;
    Json classExpr;
    if (cur().kind == TokKind::Ident) {
        if (lower(cur().text) == "class") error("anonymous classes are not supported");
        if (isReserved(lower(cur().text))) error("class name expected after 'new'");
        cls = resolveClass(cur().text);
        advance();
        if (isOp("::")) error("unexpected '::' after class name in 'new'");
    } else if (cur().kind == TokKind::Variable) {
        classExpr = varRef(cur().text).node;
        advance();
        cls = "object";
    } else {
        error("class name expected after 'new'");
    }
    std::vector<Expr> args;
    if (isOp("(")) args = parseArgs();
    Json a = Json::array();
    for (const auto& e : args) a.push(e.node);
    Json node = obj({{"type", "dynamic-memory-allocation"},
                     {"array-flag", "false"},
                     {"address-hint", nullptr},
                     {"variable-type", typeNode(cls)},
                     {"constructor-name", "__construct"},
                     {"constructor-arguments", a}});
    if (!classExpr.isNull()) node["class-expression"] = classExpr;
    return {node, cls};
}

// Class::CONST, Class::$prop, Class::method(), Class::class.
// Слева от :: может стоять и выражение ($obj::CONST, Suit::Hearts::X) — тогда classExpr != nullptr.
Parser::Expr Parser::parseStaticAccess(const std::string& cls, const Json* classExpr) {
    expectOp("::");
    std::string rc = resolveClass(cls);
    const char* ownerKey = classExpr ? "class-expression" : "class-name";
    Json owner = classExpr ? *classExpr : Json(cls);
    if (acceptKw("class")) {
        if (classExpr) return callNode("get_class", {Expr{*classExpr, cls}}, "string");  // $obj::class
        return {literal("string", rc), "string"};
    }
    if (cur().kind == TokKind::Variable) {
        std::string p = cur().text;
        advance();
        std::string t = lookupMember(rc, p, 0);
        return {obj({{"type", "member-access"},
                     {"result-type", typeNode(t)},
                     {ownerKey, owner},
                     {"member-name", "$" + p},
                     {"static", true}}),
                t};
    }
    if (acceptOp("$")) {  // Class::$$name — свойство с вычисляемым именем
        Expr inner;
        if (cur().kind == TokKind::Variable || isOp("$")) inner = parsePrimary();
        else if (acceptOp("{")) {
            inner = parseExpr();
            expectOp("}");
        } else error("variable name expected after '$'");
        return {obj({{"type", "member-access"},
                     {"result-type", typeNode("mixed")},
                     {ownerKey, owner},
                     {"member-name", nullptr},
                     {"member-expression", inner.node},
                     {"static", true}}),
                "mixed"};
    }
    if (cur().kind != TokKind::Ident) error("member name expected after '::'");
    std::string m = cur().text;
    advance();
    if (isOp("(")) {
        std::vector<Expr> args = parseArgs();
        Expr c = callNode(m, args, lookupMember(rc, m, 1));
        c.node[ownerKey] = owner;
        return c;
    }
    std::string t = lookupMember(rc, m, 2);
    return {obj({{"type", "member-access"}, {"result-type", typeNode(t)}, {ownerKey, owner}, {"member-name", m}, {"static", true}}),
            t};
}

// Строка "..." с подстановками превращается в цепочку сцеплений строк.
Parser::Expr Parser::parseTemplate(const std::string& raw) {
    std::vector<Expr> parts;
    std::string lit;
    auto flush = [&] {
        if (!lit.empty()) {
            parts.push_back({literal("string", lit), "string"});
            lit.clear();
        }
    };
    std::size_t i = 0, n = raw.size();
    while (i < n) {
        char c = raw[i];
        if (c == '\\' && i + 1 < n) {
            i = decodeEscape(raw, i, lit);
            continue;
        }
        if (c == '$' && i + 1 < n && isIdentStart(raw[i + 1])) {  // простая подстановка: $a, $a[k], $a->b
            flush();
            std::size_t j = i + 1;
            while (j < n && isIdentChar(raw[j])) ++j;
            Expr v = varRef(raw.substr(i + 1, j - i - 1));
            if (j < n && raw[j] == '[') {
                // В простой подстановке ключ — число, имя (без кавычек) или переменная: "$a[0]", "$a[key]", "$a[$i]"
                std::size_t k = raw.find(']', j);
                if (k == std::string::npos) throw std::runtime_error("unterminated array index in string interpolation");
                std::string key = raw.substr(j + 1, k - j - 1);
                auto allIdent = [](const std::string& w, std::size_t from) {
                    if (w.size() <= from || !isIdentStart(w[from])) return false;
                    return std::all_of(w.begin() + static_cast<std::ptrdiff_t>(from), w.end(), isIdentChar);
                };
                std::size_t d0 = (!key.empty() && key[0] == '-') ? 1 : 0;
                bool digits = key.size() > d0 && std::all_of(key.begin() + static_cast<std::ptrdiff_t>(d0), key.end(),
                                                             [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
                Json idx;
                if (digits) {
                    // Десятичное целое без ведущих нулей — целый ключ, иначе (например "01") — строковый.
                    bool canonical = key.size() == d0 + 1 || key[d0] != '0';
                    long long val = 0;
                    bool fits = false;
                    if (canonical) {
                        try {
                            val = std::stoll(key);
                            fits = true;
                        } catch (...) {}
                    }
                    idx = fits ? literal("integer", Json(val)) : literal("string", key);
                } else if (!key.empty() && key[0] == '$' && allIdent(key, 1)) {
                    idx = varRef(key.substr(1)).node;
                } else if (allIdent(key, 0)) {
                    idx = literal("string", key);  // "$a[key]" означает $a['key']
                } else {
                    throw std::runtime_error("invalid array index in string interpolation: [" + key + "]");
                }
                v = {obj({{"type", "index"}, {"array-expression", v.node}, {"result-type", typeNode("mixed")}, {"indices", arr({idx})}}),
                     "mixed"};
                j = k + 1;
            } else if (j + 2 < n && raw[j] == '-' && raw[j + 1] == '>' && isIdentStart(raw[j + 2])) {
                std::size_t k = j + 2;
                while (k < n && isIdentChar(raw[k])) ++k;
                std::string m = raw.substr(j + 2, k - j - 2);
                std::string t = lookupMember(v.type, m, 0);
                v = {obj({{"type", "member-access"}, {"result-type", typeNode(t)}, {"object", v.node}, {"member-name", m}}), t};
                j = k;
            }
            parts.push_back(v);
            i = j;
            continue;
        }
        if (c == '{' && i + 1 < n && raw[i + 1] == '$') {  // сложная подстановка: {$a->b['c']}
            flush();
            std::size_t k = matchBrace(raw, i);
            if (raw[k] != '}') throw std::runtime_error("unterminated {$...} in string");
            parts.push_back(parseSubExpression(raw.substr(i + 1, k - i - 1), /*variableOnly=*/true));
            i = k + 1;
            continue;
        }
        if (c == '$' && i + 1 < n && raw[i + 1] == '{') {  // устаревшая форма: "${name}", "${name[expr]}", "${expr}"
            flush();
            std::size_t k = matchBrace(raw, i + 1);
            if (raw[k] != '}') throw std::runtime_error("unterminated ${...} in string");
            std::string inner = raw.substr(i + 2, k - i - 2);
            std::size_t w = 0;
            while (w < inner.size() && isIdentChar(inner[w])) ++w;
            if (w > 0 && isIdentStart(inner[0]) && w == inner.size()) {
                parts.push_back(varRef(inner));
            } else if (w > 0 && isIdentStart(inner[0]) && inner[w] == '[') {
                parts.push_back(parseSubExpression("$" + inner, true));
            } else {
                Expr e = parseSubExpression(inner, false);
                parts.push_back({obj({{"type", "variable-variable"}, {"result-type", typeNode("mixed")}, {"argument", e.node}}), "mixed"});
            }
            i = k + 1;
            continue;
        }
        lit += c;
        ++i;
    }
    flush();
    if (parts.empty()) return {literal("string", ""), "string"};
    if (parts.size() == 1) {
        if (nodeType(parts[0].node) == "literal-constant") return parts[0];
        // "$x" — это преобразование $x в строку
        return {obj({{"type", "type-cast"}, {"result-type", typeNode("string")}, {"argument", parts[0].node}}), "string"};
    }
    Expr acc = parts[0];
    for (std::size_t k = 1; k < parts.size(); ++k)
        acc = {obj({{"type", "string-concatenation"}, {"result-type", "string"}, {"left", acc.node}, {"right", parts[k].node}}),
               "string"};
    return acc;
}

// Разбор выражения из {$...} внутри строки: отдельный лексер, тот же парсер и та же таблица символов.
Parser::Expr Parser::parseSubExpression(const std::string& src, bool variableOnly) {
    Lexer lx(src, true);
    std::vector<Token> sub = lx.tokenize();
    std::swap(toks_, sub);
    std::size_t savedPos = pos_;
    pos_ = 0;
    Expr e;
    try {
        if (variableOnly) {  // в {$...} допустима только "переменная": $a, $a[..], $a->b, $a->m(), $a::$b
            e = parsePostfix(/*allowAssign=*/false);
            std::string nt = nodeType(e.node);
            bool ok = (nt == "variable-reference" && !varNameOf(e.node).empty()) || nt == "index" ||
                      nt == "member-access" || nt == "function-call" || nt == "variable-variable";
            if (!ok) error("variable expected in string interpolation");
            if (hasAppendIndex(e.node)) error("cannot use [] for reading");
        } else {
            e = parseExpr();
        }
        if (cur().kind != TokKind::End) error("unexpected token in string interpolation");
    } catch (const std::runtime_error& ex) {
        std::swap(toks_, sub);
        pos_ = savedPos;
        // Позиция внутри подстроки бессмысленна для пользователя — номер строки добавит разбор строкового литерала.
        std::string m = ex.what();
        std::size_t p = m.find(": ");
        if (m.rfind("line ", 0) == 0 && p != std::string::npos) m = m.substr(p + 2);
        throw std::runtime_error("in string interpolation: " + m);
    } catch (...) {
        std::swap(toks_, sub);
        pos_ = savedPos;
        throw;
    }
    std::swap(toks_, sub);
    pos_ = savedPos;
    return e;
}

// Список выражений через запятую (заголовок for) -> операция "последование".
Json Parser::parseExprList(const char* closer) {
    if (isOp(closer)) return Json();
    Expr e = parseExpr();
    while (acceptOp(",")) {
        Expr r = parseExpr();
        e = {obj({{"type", "sequence"}, {"result-type", typeNode(r.type)}, {"left", e.node}, {"right", r.node}}), r.type};
    }
    return e.node;
}

// ============================================================================
//  Операторы
// ============================================================================
Json Parser::parseStatement() {
    DepthGuard guard(*this);
    const Token& t = cur();
    if (t.kind == TokKind::InlineHtml) {  // HTML вне тегов == echo "..."
        std::string text = t.text;
        advance();
        return obj({{"type", "expression-statement"},
                    {"expression", callNode("echo", {Expr{literal("string", text), "string"}}, "void").node}});
    }
    if (t.kind == TokKind::Op) {
        if (t.text == "{") return parseBlock();
        if (t.text == ";") {  // пустой оператор
            advance();
            return Json();
        }
    }
    if (t.kind == TokKind::Ident) {
        std::string kw = lower(t.text);
        if (isOp(":", 1) && kw != "else" && kw != "default" && kw != "case") return parseLabel();
        if (kw == "if") return parseIf();
        if (kw == "while") return parseWhile();
        if (kw == "do") return parseDoWhile();
        if (kw == "for") return parseFor();
        if (kw == "foreach") return parseForeach();
        if (kw == "switch") return parseSwitch();
        if (kw == "break" || kw == "continue") return parseBreakContinue();
        if (kw == "return") return parseReturn();
        if (kw == "echo") return parseEcho();
        if (kw == "global") return parseGlobalDecl();
        if (kw == "static" && peekTok(1).kind == TokKind::Variable) return parseStaticDecl();
        if (kw == "const") return parseConstDecl();
        if (kw == "goto") return parseGoto();
        if (kw == "define" && isOp("(", 1) && peekTok(2).kind == TokKind::String && isOp(",", 3)) return parseDefine();
        if (kw == "function" && (peekTok(1).kind == TokKind::Ident || (isOp("&", 1) && peekTok(2).kind == TokKind::Ident))) {
            Json f = parseFunctionDecl(nullptr);
            scope().subroutines.push(f);
            return Json();
        }
        if (((kw == "class" || kw == "interface") && peekTok(1).kind == TokKind::Ident) || kw == "abstract" ||
            kw == "final" || (kw == "readonly" && isKw("class", 1))) {
            Json c = parseClassDecl();
            scope().types.push(c);
            return Json();
        }
        if (kw == "enum" && peekTok(1).kind == TokKind::Ident) {
            Json e = parseEnumDecl();
            scope().types.push(e);
            return Json();
        }
        if (kw == "trait" && peekTok(1).kind == TokKind::Ident) error("trait declarations are not supported");
        if (kw == "try" || kw == "throw") error("exceptions (try/catch/throw) are not supported");
        if (kw == "declare") {  // declare(strict_types=1); — на структуру программы не влияет
            advance();
            expectOp("(");
            while (!acceptOp(")")) {
                if (cur().kind == TokKind::End) error("unterminated declare");
                advance();
            }
            endStatement();
            return Json();
        }
        if (kw == "namespace") {
            advance();
            while (cur().kind == TokKind::Ident) advance();
            if (isOp("{")) return parseBlock();
            endStatement();
            return Json();
        }
        if (kw == "use") {
            while (!isOp(";") && cur().kind != TokKind::End) advance();
            endStatement();
            return Json();
        }
    }
    Expr e = parseExpr();
    endStatement();
    return obj({{"type", "expression-statement"}, {"expression", e.node}});
}

Json Parser::parseBlock() {
    expectOp("{");
    Json st = Json::array();
    while (!isOp("}")) {
        if (cur().kind == TokKind::End) error("expected '}'");
        Json s = parseStatement();
        if (!s.isNull()) st.push(s);
    }
    expectOp("}");
    return compound(st);
}

// Для альтернативного синтаксиса: if (...): ... endif;
Json Parser::parseStatementsUntil(std::initializer_list<const char*> stops) {
    Json st = Json::array();
    while (true) {
        if (cur().kind == TokKind::End) error("unexpected end of file");
        bool stop = false;
        for (const char* k : stops)
            if (isKw(k)) stop = true;
        if (stop) break;
        Json s = parseStatement();
        if (!s.isNull()) st.push(s);
    }
    return compound(st);
}

Json Parser::parseIf() {
    advance();  // 'if' или 'elseif'
    expectOp("(");
    Expr c = parseExpr();
    expectOp(")");
    Json thenS, elseS;
    if (acceptOp(":")) {
        thenS = parseStatementsUntil({"elseif", "else", "endif"});
        if (isKw("elseif")) {
            elseS = parseIf();  // вложенный разбор сам поглотит endif
        } else {
            if (acceptKw("else")) {
                expectOp(":");
                elseS = parseStatementsUntil({"endif"});
            }
            expectKw("endif");
            endStatement();
        }
    } else {
        thenS = orEmpty(parseStatement());
        if (isKw("elseif")) elseS = parseIf();  // elseif == else if
        else if (acceptKw("else")) elseS = orEmpty(parseStatement());
    }
    return obj({{"type", "conditional-statement"}, {"condition", c.node}, {"if-true", thenS}, {"if-false", elseS}});
}

static Json loopNode(Json begin, Json end, Json index, Json init, Json step, Json body) {
    return obj({{"type", "loop-statement"},
                {"begin-condition", std::move(begin)},
                {"end-condition", std::move(end)},
                {"loop-index", std::move(index)},
                {"loop-initialization", std::move(init)},
                {"loop-pass-to-next-iteration", std::move(step)},
                {"loop-body", std::move(body)}});
}

// while (c) S  — цикл с предусловием
Json Parser::parseWhile() {
    ScopedSet<int> loop(loopDepth_, loopDepth_ + 1);
    advance();
    expectOp("(");
    Expr c = parseExpr();
    expectOp(")");
    Json body;
    if (acceptOp(":")) {
        body = parseStatementsUntil({"endwhile"});
        expectKw("endwhile");
        endStatement();
    } else {
        body = orEmpty(parseStatement());
    }
    return loopNode(c.node, Json(), Json(), Json(), Json(), body);
}

// do S while (c);  — цикл с постусловием (end-condition — условие продолжения, как в C)
Json Parser::parseDoWhile() {
    ScopedSet<int> loop(loopDepth_, loopDepth_ + 1);
    advance();
    Json body = orEmpty(parseStatement());
    expectKw("while");
    expectOp("(");
    Expr c = parseExpr();
    expectOp(")");
    endStatement();
    return loopNode(Json(), c.node, Json(), Json(), Json(), body);
}

// Переменная цикла for — первая переменная, которой присваивают в инициализации.
static Json findLoopIndex(const Json& n) {
    std::string t = nodeType(n);
    if (t == "sequence") {
        const Json* l = n.find("left");
        return l ? findLoopIndex(*l) : Json();
    }
    if (t.rfind("assignment", 0) == 0) {
        const Json* v = n.find("variable");
        if (v && nodeType(*v) == "variable-reference") return *v;
    }
    return Json();
}

// for (init; cond; step) S
Json Parser::parseFor() {
    ScopedSet<int> loop(loopDepth_, loopDepth_ + 1);
    advance();
    expectOp("(");
    Json init = parseExprList(";");
    expectOp(";");
    Json cond = parseExprList(";");
    expectOp(";");
    Json step = parseExprList(")");
    expectOp(")");
    Json body;
    if (acceptOp(":")) {
        body = parseStatementsUntil({"endfor"});
        expectKw("endfor");
        endStatement();
    } else {
        body = orEmpty(parseStatement());
    }
    return loopNode(cond, Json(), findLoopIndex(init), init, step, body);
}

// foreach ($coll as [$k =>] [&]$v) S
Json Parser::parseForeach() {
    ScopedSet<int> loop(loopDepth_, loopDepth_ + 1);
    advance();
    expectOp("(");
    Expr coll = parseExpr();
    expectKw("as");
    auto target = [&](bool& byRef) {
        byRef = acceptOp("&");
        if (isOp("[") || isKw("list")) error("destructuring in foreach is not supported");
        if (cur().kind != TokKind::Variable && !isOp("$")) error("variable expected in foreach");
        Expr v = parsePostfix(/*allowAssign=*/false);
        if (!isAssignable(v.node)) error("variable expected in foreach");
        if (isThis(v.node)) error("cannot re-assign $this");
        markLvalue(v.node);
        updateTargetType(v.node, "mixed");
        return v;
    };
    bool r1 = false, r2 = false;
    Expr first = target(r1);
    Json key, value;
    bool byRef = false;
    if (acceptOp("=>")) {
        Expr second = target(r2);
        key = first.node;
        value = second.node;
        byRef = r2;
    } else {
        value = first.node;
        byRef = r1;
    }
    expectOp(")");
    Json body;
    if (acceptOp(":")) {
        body = parseStatementsUntil({"endforeach"});
        expectKw("endforeach");
        endStatement();
    } else {
        body = orEmpty(parseStatement());
    }
    Json n = loopNode(Json(), Json(), value, Json(), Json(), body);
    n["foreach-collection"] = coll.node;  // расширение схемы
    n["foreach-key"] = key;
    if (byRef) n["by-reference"] = true;
    return n;
}

// switch: варианты "проваливаются" друг в друга без break, поэтому pass-control = true
Json Parser::parseSwitch() {
    ScopedSet<int> loop(loopDepth_, loopDepth_ + 1);
    advance();
    expectOp("(");
    Expr e = parseExpr();
    expectOp(")");
    bool alt = acceptOp(":");
    if (!alt) expectOp("{");
    acceptOp(";");  // PHP допускает одну ';' перед первым case
    Json variants = Json::array();
    while (true) {
        if (alt && acceptKw("endswitch")) {
            endStatement();
            break;
        }
        if (!alt && acceptOp("}")) break;
        Json label;
        if (acceptKw("case")) label = parseExpr().node;
        else if (acceptKw("default")) label = "default";
        else error("expected 'case' or 'default'");
        if (!acceptOp(":")) expectOp(";");
        Json body = Json::array();
        while (!isKw("case") && !isKw("default") && !isOp("}") && !isKw("endswitch")) {
            if (cur().kind == TokKind::End) error("unexpected end of file in switch");
            Json s = parseStatement();
            if (!s.isNull()) body.push(s);
        }
        variants.push(obj({{"type", "choose-variant"}, {"condition-label", label}, {"variant-body", compound(body)}}));
    }
    return obj({{"type", "choose-statement"}, {"pass-control", true}, {"expression-to-analyze", e.node}, {"variants", variants}});
}

// break [N]; continue [N];  — N (число уровней вложенности) записывается в label
Json Parser::parseBreakContinue() {
    bool isBreak = lower(cur().text) == "break";
    advance();
    Json lbl;
    long long levels = 1;
    if (cur().kind == TokKind::Int) {
        levels = cur().ival;
        if (levels < 1) error(std::string("'") + (isBreak ? "break" : "continue") + "' operator accepts only positive integers");
        lbl = Json(levels);
        advance();
    }
    if (loopDepth_ == 0) error(std::string("'") + (isBreak ? "break" : "continue") + "' not in the 'loop' or 'switch' context");
    if (levels > loopDepth_) error("cannot '" + std::string(isBreak ? "break" : "continue") + "' " + std::to_string(levels) + " levels");
    endStatement();
    return obj({{"type", isBreak ? "break-statement" : "continue-statement"}, {"label", lbl}});
}

Json Parser::parseReturn() {
    advance();
    Json e;
    std::string t = "void";
    const std::string& decl = scope().declaredReturn;
    if (!isOp(";") && cur().kind != TokKind::End) {
        if (decl == "void") error("a void function must not return a value");
        if (decl == "never") error("a never-returning function must not return");
        Expr x = parseExpr();
        e = x.node;
        t = x.type;
    } else if (!decl.empty() && decl != "void") {
        error(decl == "never" ? "a never-returning function must not return"
                              : "a function with return type must return a value");
    }
    scope().returnTypes.push_back(t);
    endStatement();
    return obj({{"type", "return-statement"}, {"expression", e}, {"the-last-statement", false}});
}

// echo a, b;  — вызов встроенной "функции" echo
Json Parser::parseEcho() {
    advance();
    std::vector<Expr> args;
    do {
        args.push_back(parseExpr());
    } while (acceptOp(","));
    endStatement();
    return obj({{"type", "expression-statement"}, {"expression", callNode("echo", args, "void").node}});
}

// global $a, $b;
Json Parser::parseGlobalDecl() {
    advance();
    Json decls = Json::array();
    do {
        if (cur().kind != TokKind::Variable) error("variable expected");
        if (cur().text == "this") error("cannot use $this as global variable");
        std::string v = cur().text;
        advance();
        std::string t;
        auto g = scopes_.front().vars.find(v);  // тип берём из глобальной области
        if (g != scopes_.front().vars.end()) t = g->second.type;
        bool isNew = !scope().vars.count(v);
        VarInfo& vi = scope().vars[v];
        if (isNew) scope().order.push_back(v);
        vi.explicitDecl = true;
        vi.type = t;
        vi.declType = t;
        decls.push(declarator("$" + v, t.empty() ? "mixed" : t, Json::array()));
    } while (acceptOp(","));
    endStatement();
    scope().explicitVars.push(
        obj({{"type", "variables-declaration"}, {"global", true}, {"static", false}, {"const", false}, {"declarators", decls}}));
    return Json();
}

// static $count = 0;
Json Parser::parseStaticDecl() {
    advance();
    Json decls = Json::array();
    do {
        if (cur().kind != TokKind::Variable) error("variable expected");
        if (cur().text == "this") error("cannot use $this as static variable");
        std::string v = cur().text;
        advance();
        Json init = Json::array();
        std::string t = "null";
        if (acceptOp("=")) {
            Token at = cur();
            Expr d = parseExpr();
            requireConstExpr(d, at, /*allowNew=*/true);
            init.push(d.node);
            t = d.type;
        }
        bool isNew = !scope().vars.count(v);
        VarInfo& vi = scope().vars[v];
        if (isNew) scope().order.push_back(v);
        vi.explicitDecl = true;
        vi.type = t;
        vi.declType = t;
        decls.push(declarator("$" + v, t, init));
    } while (acceptOp(","));
    endStatement();
    scope().explicitVars.push(
        obj({{"type", "variables-declaration"}, {"global", false}, {"static", true}, {"const", false}, {"declarators", decls}}));
    return Json();
}

// const PI = 3.14, E = 2.71;
Json Parser::parseConstDecl() {
    advance();
    Json decls = Json::array();
    do {
        if (cur().kind != TokKind::Ident) error("constant name expected");
        std::string cn = cur().text;
        advance();
        expectOp("=");
        Token at = cur();
        Expr v = parseExpr();
        requireConstExpr(v, at, /*allowNew=*/true);
        constTypes_[cn] = v.type;
        decls.push(declarator(cn, v.type, arr({v.node})));
    } while (acceptOp(","));
    endStatement();
    scope().constants.push(obj({{"type", "variables-declaration"},
                                {"global", scope().isMain},
                                {"static", false},
                                {"const", true},
                                {"declarators", decls}}));
    return Json();
}

// define('NAME', value);  — то же, что const
Json Parser::parseDefine() {
    advance();
    expectOp("(");
    std::string cn = cur().text;
    advance();
    expectOp(",");
    Expr v = parseExpr();
    if (acceptOp(",")) parseExpr();  // устаревший флаг case_insensitive
    expectOp(")");
    endStatement();
    constTypes_[cn] = v.type;
    scope().constants.push(obj({{"type", "variables-declaration"},
                                {"global", true},
                                {"static", false},
                                {"const", true},
                                {"declarators", arr({declarator(cn, v.type, arr({v.node}))})}}));
    return Json();
}

Json Parser::parseGoto() {
    advance();
    if (cur().kind != TokKind::Ident) error("label expected");
    std::string l = cur().text;
    scope().gotos.push_back({l, cur()});
    advance();
    endStatement();
    return obj({{"type", "goto-statement"}, {"target-label", l}});
}

Json Parser::parseLabel() {
    std::string l = cur().text;
    if (isReserved(lower(l))) error("unexpected keyword");
    if (!scope().labels.emplace(l, cur().line).second) error("label '" + l + "' already defined");
    advance();
    advance();  // ':'
    Json st;
    if (isOp("}") || cur().kind == TokKind::End) st = compound(Json::array());  // метка в конце блока
    else st = orEmpty(parseStatement());
    return obj({{"type", "label"}, {"label-name", l}, {"statement", st}});
}

// function name(Type $a, &$b, $c = 1, ...$rest): Type { ... }
Json Parser::parseFunctionDecl(ClassCtx* cls) {
    expectKw("function");
    bool returnsRef = acceptOp("&");
    if (cur().kind != TokKind::Ident) error("function name expected");
    std::string name = cur().text;
    advance();
    Scope sc;
    sc.name = name;
    scopes_.push_back(std::move(sc));
    ScopedSet<int> noLoops(loopDepth_, 0);  // break внутри функции не видит циклов снаружи

    expectOp("(");
    Json args = Json::array();
    while (!isOp(")")) {
        std::string visibility;  // продвижение свойства в конструкторе: __construct(private int $x)
        while (isKw("public") || isKw("private") || isKw("protected") || isKw("readonly")) {
            if (!isKw("readonly")) visibility = lower(cur().text);
            else if (visibility.empty()) visibility = "public";
            advance();
        }
        if (!visibility.empty() && (!cls || lower(name) != "__construct"))
            error("cannot declare promoted property outside a constructor");
        std::string ptype;
        bool hasType = false;
        if (cur().kind != TokKind::Variable && !isOp("&") && !isOp("...")) {
            ptype = parseTypeHint();
            hasType = true;
        }
        bool byRef = acceptOp("&");
        bool variadic = acceptOp("...");
        if (cur().kind != TokKind::Variable) error("parameter name expected");
        if (cur().text == "this") error("cannot use $this as parameter");
        std::string pname = cur().text;
        advance();
        Json init = Json::array();
        if (acceptOp("=")) {  // значение по умолчанию — как аргумент "конструктора" декларатора
            Token at = cur();
            Expr d = parseExpr();
            requireConstExpr(d, at, /*allowNew=*/true);
            std::string dn = nodeType(d.node);
            if (hasType && (dn == "literal-constant" || dn == "array-literal") &&
                !defaultCompatible(ptype, d.type, /*implicitNull=*/true))
                throw std::runtime_error("line " + std::to_string(at.line) + ":" + std::to_string(at.col) + ": cannot use " +
                                         phpTypeName(d.type) + " as default value for parameter $" + pname + " of type " + ptype);
            init.push(d.node);
            if (!hasType && d.type != "null") ptype = d.type;
        }
        if (variadic) ptype = "array";
        if (ptype.empty()) ptype = "mixed";
        VarInfo vi;
        vi.type = ptype;
        vi.declType = ptype;
        vi.explicitDecl = true;
        scope().vars[pname] = vi;
        scope().order.push_back(pname);
        Json d = declarator("$" + pname, ptype, init, byRef);
        if (variadic) d["variadic"] = true;
        args.push(obj({{"type", "variables-declaration"},
                       {"global", false},
                       {"static", false},
                       {"const", false},
                       {"declarators", arr({d})}}));
        if (!visibility.empty() && cls) {
            Json f = obj({{"type", "variables-declaration"},
                          {"global", false},
                          {"static", false},
                          {"const", false},
                          {"declarators", arr({declarator("$" + pname, ptype, Json::array())})}});
            f["visibility"] = visibility;
            cls->fields.push(f);
            classes_[lower(cls->name)].props[pname] = ptype;
        }
        if (!acceptOp(",")) break;
    }
    expectOp(")");

    std::string rtype;
    bool hasR = false;
    if (acceptOp(":")) {
        rtype = parseTypeHint();
        hasR = true;
        scope().declaredReturn = rtype;
    }
    std::string key = lower(name);
    if (hasR) {
        if (cls) classes_[lower(cls->name)].methods[key] = rtype;
        else funcTypes_[key] = rtype;
    }

    Json body;
    if (!acceptOp(";")) body = parseBlock();  // ';' — абстрактный метод / метод интерфейса

    if (!hasR) {  // тип результата не указан — выводим по операторам return
        const auto& rts = scope().returnTypes;
        if (key == "__construct" || key == "__destruct") rtype = "void";
        else if (rts.empty()) rtype = body.isNull() ? "mixed" : "void";
        else {
            rtype.clear();
            for (const auto& r : rts) rtype = unify(rtype, r);
        }
        if (cls) classes_[lower(cls->name)].methods.emplace(key, rtype);
        else funcTypes_.emplace(key, rtype);
    }
    Json node = finishSubroutine(name, rtype, args, body);
    if (returnsRef) node["returns-reference"] = true;
    scopes_.pop_back();
    return node;
}

// class Name extends Base implements I1, I2 { поля, константы, методы }  -> struct-type
Json Parser::parseClassDecl() {
    bool isAbstract = false, isFinal = false;
    while (isKw("abstract") || isKw("final") || isKw("readonly")) {
        if (isKw("abstract")) isAbstract = true;
        if (isKw("final")) isFinal = true;
        advance();
    }
    std::string kind = "class";
    if (acceptKw("interface")) kind = "interface";
    else expectKw("class");
    if (cur().kind != TokKind::Ident) error("class name expected");
    std::string className = cur().text;
    advance();
    ClassInfo& ci = classes_[lower(className)];
    Json bases = Json::array();
    auto readNames = [&](bool setParent) {
        do {
            if (cur().kind != TokKind::Ident) error("class name expected");
            if (setParent && ci.parent.empty()) ci.parent = cur().text;
            bases.push(typeNode(cur().text));
            advance();
        } while (acceptOp(","));
    };
    if (acceptKw("extends")) readNames(true);
    if (acceptKw("implements")) readNames(false);

    std::string savedClass = currentClass_;
    ClassCtx ctx;
    ctx.name = className;
    currentClass_ = className;

    expectOp("{");
    while (!acceptOp("}")) {
        if (cur().kind == TokKind::End) error("unexpected end of file in class body");
        std::string visibility = "public";
        bool isStatic = false, isAbs = false, hadModifier = false;
        while (true) {
            if (isKw("public") || isKw("private") || isKw("protected")) {
                visibility = lower(cur().text);
            } else if (isKw("static")) {
                isStatic = true;
            } else if (isKw("abstract")) {
                isAbs = true;
            } else if (!(isKw("final") || isKw("var") || isKw("readonly"))) {
                break;
            }
            hadModifier = true;
            advance();
        }
        if (acceptKw("const")) {
            std::string ctype;
            if (cur().kind == TokKind::Ident && (peekTok(1).kind == TokKind::Ident || isOp("|", 1)))
                ctype = parseTypeHint();  // типизированная константа: const int X = 1 (PHP 8.3)
            Json decls = Json::array();
            do {
                if (cur().kind != TokKind::Ident) error("constant name expected");
                std::string cn = cur().text;
                advance();
                expectOp("=");
                Token at = cur();
                Expr v = parseExpr();
                requireConstExpr(v, at, /*allowNew=*/false);
                std::string vn = nodeType(v.node);
                if (!ctype.empty() && (vn == "literal-constant" || vn == "array-literal") &&
                    !defaultCompatible(ctype, v.type, /*implicitNull=*/false))
                    throw std::runtime_error("line " + std::to_string(at.line) + ":" + std::to_string(at.col) + ": cannot use " +
                                             phpTypeName(v.type) + " as value for class constant " + cn + " of type " + ctype);
                decls.push(declarator(cn, ctype.empty() ? v.type : ctype, arr({v.node})));
                classes_[lower(className)].consts[cn] = v.type;
            } while (acceptOp(","));
            endStatement();
            Json f = obj({{"type", "variables-declaration"}, {"global", false}, {"static", true}, {"const", true}, {"declarators", decls}});
            f["visibility"] = visibility;
            ctx.fields.push(f);
        } else if (isKw("function")) {
            Json m = parseFunctionDecl(&ctx);
            m["visibility"] = visibility;
            if (isStatic) m["static"] = true;
            if (isAbs || kind == "interface") m["abstract"] = true;
            ctx.methods.push(m);
        } else if (isKw("use")) {
            error("traits are not supported");
        } else {  // свойства: модификатор [тип] $a [= v], $b;
            if (!hadModifier) error("property declaration requires a modifier (public, protected, private, var, static, readonly)");
            std::string ptype;
            bool hasType = false;
            if (cur().kind != TokKind::Variable) {
                ptype = parseTypeHint();
                hasType = true;
            }
            Json decls = Json::array();
            do {
                if (cur().kind != TokKind::Variable) error("property name expected");
                std::string pn = cur().text;
                advance();
                Json init = Json::array();
                std::string t = hasType ? ptype : "mixed";
                if (acceptOp("=")) {
                    Token at = cur();
                    Expr d = parseExpr();
                    requireConstExpr(d, at, /*allowNew=*/false);
                    std::string dn = nodeType(d.node);
                    if (hasType && (dn == "literal-constant" || dn == "array-literal") &&
                        !defaultCompatible(ptype, d.type, /*implicitNull=*/false))
                        throw std::runtime_error("line " + std::to_string(at.line) + ":" + std::to_string(at.col) +
                                                 ": cannot use " + phpTypeName(d.type) + " as default value for property $" +
                                                 pn + " of type " + ptype);
                    init.push(d.node);
                    if (!hasType && d.type != "null") t = d.type;
                }
                decls.push(declarator("$" + pn, t, init));
                classes_[lower(className)].props[pn] = t;
            } while (acceptOp(","));
            endStatement();
            Json f = obj({{"type", "variables-declaration"}, {"global", false}, {"static", isStatic}, {"const", false}, {"declarators", decls}});
            f["visibility"] = visibility;
            ctx.fields.push(f);
        }
    }
    currentClass_ = savedClass;

    Json node = obj({{"type", "struct-type"},
                     {"name", className},
                     {"base-structs", bases},
                     {"fields", ctx.fields},
                     {"methods", ctx.methods},
                     {"friends", Json::array()}});  // в PHP нет друзей класса
    node["kind"] = kind;
    if (isAbstract) node["abstract"] = true;
    if (isFinal) node["final"] = true;
    return node;
}

// enum Suit { case Hearts; case Spades; }        -> enumeration-type, значения 0, 1, ...
// enum Status: string { case Active = 'a'; }    -> enumeration-type со значениями 'a', ...
Json Parser::parseEnumDecl() {
    expectKw("enum");
    if (cur().kind != TokKind::Ident) error("enum name expected");
    std::string name = cur().text;
    advance();
    std::string backing;
    if (acceptOp(":")) {
        backing = parseTypeHint();
        if (backing != "integer" && backing != "string") error("enum backing type must be int or string");
    }
    if (acceptKw("implements")) {
        do {
            if (cur().kind != TokKind::Ident) error("interface name expected");
            advance();
        } while (acceptOp(","));
    }
    ClassInfo& ci = classes_[lower(name)];
    Json values = Json::array();
    long long ordinal = 0;
    expectOp("{");
    while (!acceptOp("}")) {
        if (cur().kind == TokKind::End) error("unexpected end of file in enum body");
        if (!acceptKw("case")) error("only 'case' entries are supported in enums");
        if (cur().kind != TokKind::Ident) error("enum case name expected");
        std::string cn = cur().text;
        advance();
        Json value;
        if (acceptOp("=")) {
            if (backing.empty()) error("pure enum case cannot have a value");
            Token at = cur();
            Expr ve = parseExpr();
            requireConstExpr(ve, at, /*allowNew=*/false);
            value = ve.node;
        } else {
            if (!backing.empty()) error("backed enum case must have a value");
            value = literal("integer", Json(ordinal));
        }
        value["name"] = cn;  // имя элемента перечисления (расширение схемы)
        values.push(value);
        ci.consts[cn] = name;  // Suit::Hearts имеет тип Suit
        ++ordinal;
        endStatement();
    }
    Json node = obj({{"type", "enumeration-type"}, {"name", name}, {"values", values}});
    if (!backing.empty()) node["backing-type"] = typeNode(backing);
    return node;
}
