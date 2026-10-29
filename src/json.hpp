#pragma once
// Минимальная библиотека JSON: построение дерева значений и печать.
// Внешние библиотеки не используются, чтобы проект собирался "из коробки".

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class Json {
public:
    enum class Kind { Null, Bool, Int, Real, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : kind_(Kind::Bool), bool_(v) {}
    Json(int v) : kind_(Kind::Int), int_(v) {}
    Json(long long v) : kind_(Kind::Int), int_(v) {}
    Json(double v) : kind_(Kind::Real), real_(v) {}
    Json(const char* v) : kind_(Kind::String), str_(v) {}
    Json(std::string v) : kind_(Kind::String), str_(std::move(v)) {}

    static Json array() { Json j; j.kind_ = Kind::Array; return j; }
    static Json object() { Json j; j.kind_ = Kind::Object; return j; }

    Kind kind() const { return kind_; }
    bool isNull() const { return kind_ == Kind::Null; }
    bool isString() const { return kind_ == Kind::String; }
    bool isInt() const { return kind_ == Kind::Int; }
    bool isReal() const { return kind_ == Kind::Real; }
    const std::string& str() const { return str_; }
    long long asInt() const { return int_; }
    double asReal() const { return real_; }

    // Объект: доступ по ключу. Отсутствующий ключ создаётся, порядок ключей сохраняется.
    Json& operator[](const std::string& key) {
        if (kind_ == Kind::Null) kind_ = Kind::Object;
        if (kind_ != Kind::Object) throw std::logic_error("Json: not an object");
        for (auto& kv : obj_)
            if (kv.first == key) return kv.second;
        obj_.emplace_back(key, Json());
        return obj_.back().second;
    }
    const Json* find(const std::string& key) const {
        for (const auto& kv : obj_)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    Json* find(const std::string& key) {
        for (auto& kv : obj_)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    // Массив
    void push(Json v) {
        if (kind_ == Kind::Null) kind_ = Kind::Array;
        if (kind_ != Kind::Array) throw std::logic_error("Json: not an array");
        arr_.push_back(std::move(v));
    }
    std::size_t size() const { return kind_ == Kind::Array ? arr_.size() : obj_.size(); }
    Json& back() { return arr_.back(); }
    std::vector<Json>& items() { return arr_; }
    const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }
    const std::vector<Json>& items() const { return arr_; }

    std::string dump(int indent = 2) const {
        std::string out;
        dumpTo(out, indent, 0);
        out += '\n';
        return out;
    }

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    long long int_ = 0;
    double real_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;

    static void pad(std::string& out, int indent, int level) {
        out.append(static_cast<std::size_t>(indent * level), ' ');
    }

    static std::string formatReal(double v) {
        if (!std::isfinite(v)) return "null";  // в JSON нет INF/NAN
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.15g", v);
        if (std::strtod(buf, nullptr) != v) std::snprintf(buf, sizeof buf, "%.17g", v);
        std::string s = buf;
        if (s.find_first_of(".eE") == std::string::npos) s += ".0";  // чтобы было видно, что это real
        return s;
    }

    // Длина корректной UTF-8 последовательности, начинающейся в s[i], или 0.
    static std::size_t utf8Len(const std::string& s, std::size_t i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = (c >= 0xF0 && c <= 0xF4) ? 4 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xC2 && c <= 0xDF) ? 2 : 0;
        if (len == 0 || i + len > s.size()) return 0;
        for (std::size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
        return len;
    }

    static void escape(std::string& out, const std::string& s) {
        out += '"';
        for (std::size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20 || c == 0x7F) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    } else if (c < 0x80) {
                        out += static_cast<char>(c);
                    } else if (std::size_t len = utf8Len(s, i)) {
                        out.append(s, i, len);  // корректный UTF-8 (например, кириллица) — как есть
                        i += len - 1;
                    } else {
                        char buf[8];  // некорректный байт — как символ Latin-1
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    }
            }
        }
        out += '"';
    }

    void dumpTo(std::string& out, int indent, int level) const {
        switch (kind_) {
            case Kind::Null: out += "null"; break;
            case Kind::Bool: out += bool_ ? "true" : "false"; break;
            case Kind::Int: out += std::to_string(int_); break;
            case Kind::Real: out += formatReal(real_); break;
            case Kind::String: escape(out, str_); break;
            case Kind::Array:
                if (arr_.empty()) { out += "[]"; break; }
                out += "[\n";
                for (std::size_t k = 0; k < arr_.size(); ++k) {
                    pad(out, indent, level + 1);
                    arr_[k].dumpTo(out, indent, level + 1);
                    out += k + 1 < arr_.size() ? ",\n" : "\n";
                }
                pad(out, indent, level);
                out += ']';
                break;
            case Kind::Object:
                if (obj_.empty()) { out += "{}"; break; }
                out += "{\n";
                for (std::size_t k = 0; k < obj_.size(); ++k) {
                    pad(out, indent, level + 1);
                    escape(out, obj_[k].first);
                    out += ": ";
                    obj_[k].second.dumpTo(out, indent, level + 1);
                    out += k + 1 < obj_.size() ? ",\n" : "\n";
                }
                pad(out, indent, level);
                out += '}';
                break;
        }
    }
};
