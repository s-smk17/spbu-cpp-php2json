#include "lexer.hpp"

#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

bool isIdentStart(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalpha(u) || c == '_' || u >= 0x80;  // PHP допускает байты >= 0x80 (UTF-8) в именах
}

bool isIdentChar(char c) {
    return isIdentStart(c) || std::isdigit(static_cast<unsigned char>(c));
}

static bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

Lexer::Lexer(std::string source, bool startInPhp) : s_(std::move(source)), php_(startInPhp) {}

void Lexer::advance(std::size_t n) {
    while (n-- > 0 && i_ < s_.size()) {
        if (s_[i_] == '\n') { ++line_; col_ = 1; }
        else ++col_;
        ++i_;
    }
}

bool Lexer::startsWith(const char* p) const {
    return s_.compare(i_, std::strlen(p), p) == 0;
}

// "<?php" — открывающий тег, только если за ним пробельный символ или конец файла.
bool Lexer::isOpenTag() const {
    if (!startsWithCI("<?php")) return false;
    char c = peek(5);
    return c == '\0' || std::isspace(static_cast<unsigned char>(c));
}

bool Lexer::startsWithCI(const char* p) const {
    for (std::size_t k = 0; p[k]; ++k) {
        if (i_ + k >= s_.size()) return false;
        if (std::tolower(static_cast<unsigned char>(s_[i_ + k])) != std::tolower(static_cast<unsigned char>(p[k])))
            return false;
    }
    return true;
}

void Lexer::error(const std::string& msg) const {
    throw std::runtime_error("line " + std::to_string(line_) + ":" + std::to_string(col_) + ": " + msg);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    while (i_ < s_.size()) {
        if (!php_) {
            // Режим HTML: всё до <?php или <?= выводится как есть (echo).
            Token html;
            html.kind = TokKind::InlineHtml;
            html.line = line_;
            html.col = col_;
            std::size_t start = i_;
            while (i_ < s_.size() && !isOpenTag() && !startsWith("<?=")) advance();
            if (i_ > start) {
                html.text = s_.substr(start, i_ - start);
                out.push_back(html);
            }
            if (i_ >= s_.size()) break;
            if (startsWith("<?=")) {  // <?= expr ?>  ==  <?php echo expr; ?>
                Token e;
                e.kind = TokKind::Ident;
                e.text = "echo";
                e.line = line_;
                e.col = col_;
                advance(3);
                out.push_back(e);
            } else {
                advance(5);
            }
            php_ = true;
            continue;
        }

        skipSpaceAndComments();
        if (i_ >= s_.size()) break;

        if (startsWith("?>")) {  // закрывающий тег действует как ';'
            Token semi;
            semi.kind = TokKind::Op;
            semi.text = ";";
            semi.line = line_;
            semi.col = col_;
            out.push_back(semi);
            advance(2);
            if (peek() == '\n') advance();
            else if (peek() == '\r' && peek(1) == '\n') advance(2);
            php_ = false;
            continue;
        }

        int line = line_, col = col_;
        char c = peek();
        Token t;
        if ((c == 'b' || c == 'B') && (peek(1) == '\'' || peek(1) == '"' || (peek(1) == '<' && peek(2) == '<' && peek(3) == '<'))) {
            advance();  // b"..." — "бинарная" строка, в PHP то же самое, что обычная
            c = peek();
        }
        if (c == '$' && isIdentStart(peek(1))) {
            advance();
            t.kind = TokKind::Variable;
            t.text = readIdent();
        } else if (isIdentStart(c) || (c == '\\' && isIdentStart(peek(1)))) {
            // Имя, возможно с пространством имён: \Foo\Bar -> Foo\Bar
            t.kind = TokKind::Ident;
            if (c == '\\') advance();
            t.text = readIdent();
            while (peek() == '\\' && isIdentStart(peek(1))) {
                advance();
                t.text += '\\';
                t.text += readIdent();
            }
        } else if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
            t = lexNumber();
        } else if (c == '\'') {
            t = lexSingleQuoted();
        } else if (c == '"') {
            t = lexDoubleQuoted();
        } else if (startsWith("<<<")) {
            t = lexHeredoc();
        } else if (c == '`') {
            error("backtick (shell) operator is not supported");
        } else {
            t = lexOperator();
        }
        t.line = line;
        t.col = col;
        out.push_back(t);
    }
    Token end;
    end.kind = TokKind::End;
    end.line = line_;
    end.col = col_;
    out.push_back(end);
    return out;
}

void Lexer::skipSpaceAndComments() {
    while (i_ < s_.size()) {
        char c = peek();
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
        } else if (c == '#' || (c == '/' && peek(1) == '/')) {
            // Однострочный комментарий заканчивается на конце строки или перед ?>
            while (i_ < s_.size() && peek() != '\n' && !startsWith("?>")) advance();
        } else if (c == '/' && peek(1) == '*') {
            advance(2);
            while (i_ < s_.size() && !startsWith("*/")) advance();
            if (i_ >= s_.size()) error("unterminated comment");
            advance(2);
        } else {
            break;
        }
    }
}

std::string Lexer::readIdent() {
    std::string r;
    while (i_ < s_.size() && isIdentChar(peek())) {
        r += peek();
        advance();
    }
    return r;
}

Token Lexer::lexNumber() {
    Token t;
    std::string digits;
    int base = 10;
    bool isReal = false;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) { base = 16; advance(2); }
    else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) { base = 2; advance(2); }
    else if (peek() == '0' && (peek(1) == 'o' || peek(1) == 'O')) { base = 8; advance(2); }

    if (base != 10) {
        while (std::isxdigit(static_cast<unsigned char>(peek())) || peek() == '_') {
            if (peek() != '_') digits += peek();
            advance();
        }
        if (digits.empty()) error("malformed number literal");
    } else {
        auto readDigits = [&] {
            while (isDigit(peek()) || (peek() == '_' && isDigit(peek(1)))) {
                if (peek() != '_') digits += peek();
                advance();
            }
        };
        readDigits();
        if (peek() == '.' && isDigit(peek(1))) {  // дробная часть
            isReal = true;
            digits += '.';
            advance();
            readDigits();
        } else if (peek() == '.' && !digits.empty()) {  // "3." — тоже вещественное, как в PHP
            isReal = true;
            digits += '.';
            advance();
        }
        if ((peek() == 'e' || peek() == 'E') &&
            (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))))) {  // порядок
            isReal = true;
            digits += 'e';
            advance();
            if (peek() == '+' || peek() == '-') { digits += peek(); advance(); }
            readDigits();
        }
        if (!isReal && digits.size() > 1 && digits[0] == '0') {  // 0755 — восьмеричное
            base = 8;
            digits = digits.substr(1);
        }
    }

    if (isReal) {
        t.kind = TokKind::Real;
        t.rval = std::strtod(digits.c_str(), nullptr);
        t.text = digits;
        return t;
    }

    // Целое: накапливаем с контролем переполнения. При переполнении PHP превращает число в float.
    unsigned long long acc = 0;
    long double approx = 0;
    bool overflow = false;
    for (char d : digits) {
        int v = isDigit(d) ? d - '0' : std::tolower(static_cast<unsigned char>(d)) - 'a' + 10;
        if (v >= base) error("invalid digit in number literal");
        approx = approx * base + v;
        if (!overflow) {
            if (acc > (static_cast<unsigned long long>(LLONG_MAX) - v) / base) overflow = true;
            else acc = acc * base + v;
        }
    }
    if (overflow) {
        t.kind = TokKind::Real;
        t.rval = static_cast<double>(approx);
    } else {
        t.kind = TokKind::Int;
        t.ival = static_cast<long long>(acc);
    }
    t.text = digits;
    return t;
}

Token Lexer::lexSingleQuoted() {
    // В '...' экранируются только \' и \\ — остальное буквально.
    Token t;
    advance();
    std::string v;
    while (true) {
        if (i_ >= s_.size()) error("unterminated string");
        char c = peek();
        if (c == '\\' && (peek(1) == '\\' || peek(1) == '\'')) {
            v += peek(1);
            advance(2);
        } else if (c == '\'') {
            advance();
            break;
        } else {
            v += c;
            advance();
        }
    }
    t.kind = TokKind::String;
    t.text = v;
    return t;
}

Token Lexer::lexDoubleQuoted() {
    // Сохраняем сырой текст: escape-последовательности и подстановки $var / {$expr} разбирает парсер.
    Token t;
    advance();
    std::size_t start = i_;
    while (true) {
        if (i_ >= s_.size()) error("unterminated string");
        char c = peek();
        if (c == '\\') { advance(2); continue; }
        if (c == '"') break;
        if (c == '{' && peek(1) == '$') { skipBraced(); continue; }
        if (c == '$' && peek(1) == '{') { advance(); skipBraced(); continue; }  // "${name}"
        advance();
    }
    t.text = s_.substr(start, i_ - start);
    advance();
    t.kind = TokKind::Template;
    return t;
}

void Lexer::skipBraced() {
    // Пропуск {$ ... } с учётом вложенных скобок и строк внутри: "{$a["key"]}"
    int depth = 0;
    do {
        if (i_ >= s_.size()) error("unterminated {$...} in string");
        char c = peek();
        if (c == '{') { ++depth; advance(); }
        else if (c == '}') { --depth; advance(); }
        else if (c == '\'' || c == '"') skipQuoted(c);
        else advance();
    } while (depth > 0);
}

void Lexer::skipQuoted(char q) {
    advance();
    while (i_ < s_.size() && peek() != q) {
        if (peek() == '\\') advance();
        advance();
    }
    if (i_ >= s_.size()) error("unterminated string");
    advance();
}

Token Lexer::lexHeredoc() {
    // <<<ID ... ID  (heredoc, с подстановками)   <<<'ID' ... ID  (nowdoc, без подстановок)
    Token t;
    advance(3);
    while (peek() == ' ' || peek() == '\t') advance();
    char q = 0;
    if (peek() == '\'' || peek() == '"') { q = peek(); advance(); }
    if (!isIdentStart(peek())) error("invalid heredoc label");
    std::string label = readIdent();
    if (q) {
        if (peek() != q) error("invalid heredoc label");
        advance();
    }
    if (peek() == '\r') advance();
    if (peek() != '\n') error("newline expected after heredoc label");
    advance();

    std::vector<std::string> lines;
    std::size_t indent = 0;
    while (true) {
        if (i_ >= s_.size()) error("unterminated heredoc");
        std::size_t ls = i_;
        std::size_t le = s_.find('\n', i_);
        if (le == std::string::npos) le = s_.size();
        std::string ln = s_.substr(ls, le - ls);
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        std::size_t ws = 0;
        while (ws < ln.size() && (ln[ws] == ' ' || ln[ws] == '\t')) ++ws;
        if (ln.compare(ws, label.size(), label) == 0 &&
            (ws + label.size() == ln.size() || !isIdentChar(ln[ws + label.size()]))) {
            indent = ws;  // отступ закрывающей метки снимается со всех строк (PHP 7.3+)
            advance(ws + label.size());
            break;
        }
        lines.push_back(ln);
        advance(le - ls + (le < s_.size() ? 1 : 0));
    }
    std::string body;
    int braceDepth = 0;  // строки, начинающиеся внутри {$ ... }, — часть выражения, отступ у них не снимается
    for (std::size_t k = 0; k < lines.size(); ++k) {
        const std::string& ln = lines[k];
        std::size_t cut = 0;
        if (braceDepth == 0) {
            while (cut < indent && cut < ln.size() && (ln[cut] == ' ' || ln[cut] == '\t')) ++cut;
            if (cut < indent && cut < ln.size())  // непустая строка левее закрывающей метки — ошибка в PHP
                throw std::runtime_error("heredoc body line " + std::to_string(k + 1) +
                                         ": indentation is less than that of the closing marker");
        }
        if (k) body += '\n';
        body += ln.substr(cut);
        if (q == '\'') continue;  // в nowdoc подстановок нет
        for (std::size_t p = 0; p < ln.size(); ++p) {
            char ch = ln[p];
            if (braceDepth == 0) {
                if (ch == '\\') ++p;
                else if (ch == '{' && p + 1 < ln.size() && ln[p + 1] == '$') { braceDepth = 1; ++p; }
            } else if (ch == '{') ++braceDepth;
            else if (ch == '}') --braceDepth;
        }
    }
    t.kind = (q == '\'') ? TokKind::String : TokKind::Template;
    t.text = body;
    return t;
}

Token Lexer::lexOperator() {
    // Принцип "самого длинного совпадения": сначала 3-символьные, потом 2- и 1-символьные.
    static const char* ops3[] = {"<<=", ">>=", "**=", "...", "<=>", "===", "!==", "?\?=", "?->"};
    static const char* ops2[] = {"++", "--", "->", "=>", "::", "==", "!=", "<>", "<=", ">=", "&&", "||", "??",
                                 "+=", "-=", "*=", "/=", ".=", "%=", "&=", "|=", "^=", "<<", ">>", "**"};
    Token t;
    t.kind = TokKind::Op;
    for (const char* op : ops3)
        if (startsWith(op)) { t.text = op; advance(3); return t; }
    for (const char* op : ops2)
        if (startsWith(op)) {
            t.text = op;
            if (t.text == "<>") t.text = "!=";  // <> — синоним !=
            advance(2);
            return t;
        }
    char c = peek();
    if (c != '\0' && std::strchr("+-*/%=<>!.,;()[]{}?:&|^~@$", c)) {
        t.text = std::string(1, c);
        advance();
        return t;
    }
    error(std::string("unexpected character '") + c + "'");
}
