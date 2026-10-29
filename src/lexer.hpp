#pragma once
// Лексический анализатор PHP: превращает исходный текст в последовательность лексем.

#include <string>
#include <vector>

enum class TokKind {
    End,         // конец входа
    InlineHtml,  // текст вне <?php ... ?>
    Variable,    // $name (text = name без $)
    Ident,       // идентификатор или ключевое слово
    Int,         // целое число (значение в ival)
    Real,        // вещественное число (значение в rval)
    String,      // строка без подстановок: '...' или nowdoc (text = готовое значение)
    Template,    // строка с подстановками: "..." или heredoc (text = сырой текст)
    Op           // оператор или знак пунктуации
};

struct Token {
    TokKind kind = TokKind::End;
    std::string text;
    long long ival = 0;
    double rval = 0.0;
    int line = 1;
    int col = 1;
};

bool isIdentStart(char c);
bool isIdentChar(char c);

class Lexer {
public:
    // startInPhp = true — разбирать текст сразу как PHP-код (без <?php),
    // используется для подстановок вида "{$expr}" внутри строк.
    explicit Lexer(std::string source, bool startInPhp = false);
    std::vector<Token> tokenize();

private:
    std::string s_;
    std::size_t i_ = 0;
    int line_ = 1;
    int col_ = 1;
    bool php_;

    char peek(std::size_t k = 0) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }
    void advance(std::size_t n = 1);
    bool startsWith(const char* p) const;
    bool startsWithCI(const char* p) const;
    bool isOpenTag() const;
    [[noreturn]] void error(const std::string& msg) const;

    void skipSpaceAndComments();
    std::string readIdent();
    Token lexNumber();
    Token lexSingleQuoted();
    Token lexDoubleQuoted();
    Token lexHeredoc();
    Token lexOperator();
    void skipBraced();
    void skipQuoted(char q);
};
