// json.h — a small JSON reader and writer (RFC 8259).
//
// Scenes save as JSON, and editor commands are JSON values too: the web UI and
// (later) the AI send commands in this form, so one parser and one validation path
// serve every caller. Objects keep their keys in insertion order, which makes
// saved files stable and diff-friendly.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;  // null
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(double n) : type_(Type::Number), number_(n) {}
    Json(int n) : type_(Type::Number), number_(n) {}
    Json(int64_t n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Json(uint32_t n) : type_(Type::Number), number_(n) {}
    Json(const char* s) : type_(Type::String), string_(s) {}
    Json(std::string s) : type_(Type::String), string_(std::move(s)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    // A float stored so it writes back as the shortest decimal that reads back as
    // exactly the same float: 0.1f saves as 0.1, not 0.10000000149011612.
    static Json fromFloat(float f);

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const { return bool_; }
    double asNumber() const { return number_; }
    const std::string& asString() const { return string_; }

    // Arrays.
    size_t size() const { return isArray() ? items_.size() : isObject() ? members_.size() : 0; }
    const Json& operator[](size_t i) const { return items_[i]; }
    Json& push(Json value) { items_.push_back(std::move(value)); return items_.back(); }

    // Objects. find() returns nullptr if the key is missing; set() replaces an
    // existing key in place (keeping its position) or appends.
    const Json* find(const std::string& key) const;
    Json& set(const std::string& key, Json value);
    const std::vector<std::pair<std::string, Json>>& members() const { return members_; }

    // Compact by default; indent > 0 pretty-prints with that many spaces per level.
    std::string dump(int indent = 0) const;

    // Parses a whole document. Returns false and fills `error` (with an offset)
    // for anything malformed, including trailing garbage and non-finite numbers.
    static bool parse(const std::string& text, Json& out, std::string& error);

    bool operator==(const Json& o) const;
    bool operator!=(const Json& o) const { return !(*this == o); }

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};
