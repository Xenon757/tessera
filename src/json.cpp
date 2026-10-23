#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kMaxDepth = 256;  // nesting limit: deep input can't overflow the stack

// Shortest "%.Ng" text that reads back as exactly `value` (double).
std::string formatNumber(double value) {
    if (value == std::floor(value) && std::fabs(value) < 9007199254740992.0) {  // exact integer below 2^53
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", value);
        return std::string(buf) == "-0" ? "0" : buf;
    }
    char buf[40];
    for (int precision = 15; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, value);
        if (std::strtod(buf, nullptr) == value) break;
    }
    return buf;
}

void appendEscaped(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {  // other control characters must be escaped
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);  // UTF-8 passes through unchanged
                }
        }
    }
    out += '"';
}

struct Parser {
    explicit Parser(const std::string& t) : text(t) {}
    const std::string& text;
    size_t pos = 0;
    std::string error;

    bool fail(const char* what) {
        if (error.empty()) error = std::string(what) + " at offset " + std::to_string(pos);
        return false;
    }
    void skipSpace() {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) ++pos;
    }
    bool literal(const char* word) {
        const size_t n = std::strlen(word);
        if (text.compare(pos, n, word) != 0) return fail("invalid literal");
        pos += n;
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > kMaxDepth) return fail("nested too deeply");
        skipSpace();
        if (pos >= text.size()) return fail("unexpected end of input");
        switch (text[pos]) {
            case 'n': out = Json(); return literal("null");
            case 't': out = Json(true); return literal("true");
            case 'f': out = Json(false); return literal("false");
            case '"': {
                std::string s;
                if (!string(s)) return false;
                out = Json(std::move(s));
                return true;
            }
            case '[': return array(out, depth);
            case '{': return object(out, depth);
            default: return number(out);
        }
    }

    // JSON's number grammar: -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
    bool number(Json& out) {
        const size_t start = pos;
        auto digits = [&] {
            const size_t s = pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            return pos > s;
        };
        if (pos < text.size() && text[pos] == '-') ++pos;
        if (pos < text.size() && text[pos] == '0') ++pos;
        else if (!digits()) return fail("invalid number");
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            if (!digits()) return fail("invalid number");
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
            if (!digits()) return fail("invalid number");
        }
        const double v = std::strtod(text.substr(start, pos - start).c_str(), nullptr);
        if (!std::isfinite(v)) return fail("number out of range");
        out = Json(v);
        return true;
    }

    bool hex4(uint32_t& code) {
        if (pos + 4 > text.size()) return fail("truncated \\u escape");
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[pos++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= c - '0';
            else if (c >= 'a' && c <= 'f') code |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') code |= c - 'A' + 10;
            else return fail("bad hex digit in \\u escape");
        }
        return true;
    }

    static void appendUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string& out) {
        ++pos;  // opening quote
        while (true) {
            if (pos >= text.size()) return fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(text[pos++]);
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (pos >= text.size()) return fail("unterminated escape");
            switch (text[pos++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    // Characters above U+FFFF arrive as a UTF-16 surrogate pair:
                    // \uD8xx-\uDBxx (high) then \uDCxx-\uDFxx (low).
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t low;
                        if (text.compare(pos, 2, "\\u") != 0) return fail("unpaired surrogate");
                        pos += 2;
                        if (!hex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return fail("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    bool array(Json& out, int depth) {
        out = Json::array();
        ++pos;
        skipSpace();
        if (pos < text.size() && text[pos] == ']') return ++pos, true;
        while (true) {
            Json item;
            if (!value(item, depth + 1)) return false;
            out.push(std::move(item));
            skipSpace();
            if (pos >= text.size()) return fail("unterminated array");
            if (text[pos] == ',') { ++pos; continue; }
            if (text[pos] == ']') return ++pos, true;
            return fail("expected , or ]");
        }
    }

    bool object(Json& out, int depth) {
        out = Json::object();
        ++pos;
        skipSpace();
        if (pos < text.size() && text[pos] == '}') return ++pos, true;
        while (true) {
            skipSpace();
            if (pos >= text.size() || text[pos] != '"') return fail("expected a key");
            std::string key;
            if (!string(key)) return false;
            skipSpace();
            if (pos >= text.size() || text[pos] != ':') return fail("expected :");
            ++pos;
            Json item;
            if (!value(item, depth + 1)) return false;
            out.set(key, std::move(item));  // a repeated key: the last one wins
            skipSpace();
            if (pos >= text.size()) return fail("unterminated object");
            if (text[pos] == ',') { ++pos; continue; }
            if (text[pos] == '}') return ++pos, true;
            return fail("expected , or }");
        }
    }
};

}  // namespace

Json Json::fromFloat(float f) {
    // Find the fewest significant digits (6 to 9) that read back as exactly f, then
    // store that decimal's double. dump() then prints that same short decimal.
    char buf[32];
    for (int precision = 6; precision <= 9; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, static_cast<double>(f));
        if (std::strtof(buf, nullptr) == f) break;
    }
    return Json(std::strtod(buf, nullptr));
}

const Json* Json::find(const std::string& key) const {
    for (const auto& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

Json& Json::set(const std::string& key, Json value) {
    for (auto& m : members_)
        if (m.first == key) return m.second = std::move(value);
    members_.emplace_back(key, std::move(value));
    return members_.back().second;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * d), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: out += std::isfinite(number_) ? formatNumber(number_) : "null"; break;
        case Type::String: appendEscaped(out, string_); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < items_.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                items_[i].dumpTo(out, indent, depth + 1);
            }
            if (!items_.empty()) newline(depth);
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < members_.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                appendEscaped(out, members_[i].first);
                out += indent > 0 ? ": " : ":";
                members_[i].second.dumpTo(out, indent, depth + 1);
            }
            if (!members_.empty()) newline(depth);
            out += '}';
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

bool Json::parse(const std::string& text, Json& out, std::string& error) {
    Parser p(text);
    Json result;
    if (!p.value(result, 0)) return error = p.error, false;
    p.skipSpace();
    if (p.pos != text.size()) return error = "trailing characters at offset " + std::to_string(p.pos), false;
    out = std::move(result);
    return true;
}

bool Json::operator==(const Json& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == o.bool_;
        case Type::Number: return number_ == o.number_;
        case Type::String: return string_ == o.string_;
        case Type::Array: return items_ == o.items_;
        case Type::Object: return members_ == o.members_;
    }
    return false;
}
