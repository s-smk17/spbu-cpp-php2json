#pragma once
// Синтаксический анализатор PHP (рекурсивный спуск) с синтаксически управляемой
// трансляцией: каждое правило грамматики сразу строит соответствующий JSON-узел.
// Попутно ведётся таблица символов и выводятся типы (PHP типизирован динамически).

#include <map>
#include <string>
#include <vector>

#include "json.hpp"
#include "lexer.hpp"

struct Options {
    bool shortTypes = false;  // печатать типы строками "integer" вместо {"type":"builtin-type",...}
};

class Parser {
public:
    Parser(std::vector<Token> tokens, Options options);
    Json parseProgram();

private:
    // Результат разбора выражения: JSON-узел и выведенный статический тип.
    struct Expr {
        Json node;
        std::string type;
    };
    // Сведения о переменной в области видимости.
    struct VarInfo {
        std::string type;      // тип в текущей точке программы ("" — неизвестен)
        std::string declType;  // тип для декларации: объединение всех присвоенных типов
        bool explicitDecl = false;  // параметр / global / static — объявлен явно, не попадает в неявные
    };
    // Область видимости = одна подпрограмма (в PHP у блоков нет своей области).
    struct Scope {
        std::string name;
        bool isMain = false;
        std::map<std::string, VarInfo> vars;
        std::vector<std::string> order;  // порядок первого появления переменных
        Json constants = Json::array();
        Json types = Json::array();
        Json explicitVars = Json::array();
        Json subroutines = Json::array();
        std::vector<std::string> returnTypes;
        std::string declaredReturn;  // объявленный тип результата ("" — не указан)
        std::map<std::string, int> labels;                  // метка -> строка объявления
        std::vector<std::pair<std::string, Token>> gotos;   // переходы для проверки в конце подпрограммы
    };
    struct ClassInfo {
        std::string parent;
        std::map<std::string, std::string> props, methods, consts;
    };
    struct ClassCtx {
        std::string name;
        Json fields = Json::array();
        Json methods = Json::array();
    };

    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    Options opt_;
    std::vector<Scope> scopes_;
    std::map<std::string, std::string> funcTypes_;   // имя функции (нижний регистр) -> тип результата
    std::map<std::string, std::string> constTypes_;  // имя константы -> тип
    std::map<std::string, ClassInfo> classes_;       // имя класса (нижний регистр) -> сведения
    std::string currentClass_;

    // Ограничение глубины рекурсии: защита от переполнения стека на патологическом входе.
    static constexpr int kMaxDepth = 500;
    int depth_ = 0;
    int loopDepth_ = 0;          // вложенность циклов и switch — для проверки break N / continue N
    bool writeContext_ = false;  // разбирается операнд ++/-- или =& — там допустимо $a[]
    // Временная установка значения переменной на время разбора (восстанавливается в деструкторе).
    template <class T>
    struct ScopedSet {
        T& ref;
        T saved;
        ScopedSet(T& r, T v) : ref(r), saved(r) { ref = v; }
        ~ScopedSet() { ref = saved; }
        ScopedSet(const ScopedSet&) = delete;
        ScopedSet& operator=(const ScopedSet&) = delete;
    };
    void requireConstExpr(const Expr& e, const Token& at, bool allowNew) const;
    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& parser) : p(parser) {
            if (p.depth_ >= kMaxDepth) p.error("nesting is too deep");
            ++p.depth_;
        }
        ~DepthGuard() { --p.depth_; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
    };

    // --- работа с лексемами ---
    const Token& cur() const { return toks_[pos_]; }
    const Token& peekTok(std::size_t k) const;
    void advance();
    bool isOp(const char* op, std::size_t k = 0) const;
    bool isKw(const char* kw, std::size_t k = 0) const;
    bool acceptOp(const char* op);
    bool acceptKw(const char* kw);
    void expectOp(const char* op);
    void expectKw(const char* kw);
    void endStatement();
    [[noreturn]] void error(const std::string& msg) const;

    // --- типы и таблица символов ---
    Json typeNode(const std::string& t) const;
    std::string parseTypeHint();
    std::string resolveClass(const std::string& name) const;
    std::string lookupMember(std::string cls, const std::string& name, int which) const;
    std::string functionType(const std::string& lname) const;
    Scope& scope() { return scopes_.back(); }
    Expr varRef(const std::string& name);
    Expr constRef(const std::string& name);
    void setVarType(const std::string& name, const std::string& t);
    void markLvalue(Json& n);
    void updateTargetType(Json& n, const std::string& t);
    void prescan();

    // --- построители узлов ---
    Json declarator(const std::string& name, const std::string& type, Json init, bool ref = false) const;
    Json literal(const char* ctype, Json value) const;
    Expr callNode(const std::string& name, const std::vector<Expr>& args, const std::string& type) const;
    Json finishSubroutine(const std::string& name, const std::string& rtype, Json args, Json body);

    // --- выражения (от низшего приоритета к высшему) ---
    Expr parseExpr();             // or
    Expr parseKeywordXor();       // xor
    Expr parseKeywordAnd();       // and
    Expr parseTernary();          // ?:
    Expr parseBinary(int minPrec);  // ?? || && | ^ & == < . << + * instanceof
    Expr makeBinary(const std::string& op, const Expr& l, const Expr& r);
    Json parseClassRef();
    Expr parseUnary();            // ! ~ - + ++ -- (cast) @
    Expr parsePow();              // **
    Expr parsePostfix(bool allowAssign = true);  // [] -> :: () ++ --  и присваивание
    Expr parsePrimary();
    Expr parseAssignment(Expr target);
    Expr parseArrayLiteral(const char* close);
    Expr parseNew();
    Expr parseStaticAccess(const std::string& cls, const Json* classExpr = nullptr);
    Expr makeCall(const std::string& name);
    std::vector<Expr> parseArgs();
    Expr parseTemplate(const std::string& raw);
    Expr parseSubExpression(const std::string& src, bool variableOnly);
    Json parseExprList(const char* closer);

    // --- операторы ---
    Json parseStatement();
    Json parseBlock();
    Json parseStatementsUntil(std::initializer_list<const char*> stops);
    Json parseIf();
    Json parseWhile();
    Json parseDoWhile();
    Json parseFor();
    Json parseForeach();
    Json parseSwitch();
    Json parseBreakContinue();
    Json parseReturn();
    Json parseEcho();
    Json parseGlobalDecl();
    Json parseStaticDecl();
    Json parseConstDecl();
    Json parseDefine();
    Json parseGoto();
    Json parseLabel();
    Json parseFunctionDecl(ClassCtx* cls);
    Json parseClassDecl();
    Json parseEnumDecl();
};
