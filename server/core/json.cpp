#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace asrd {

std::string json_quote(const std::string & s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

std::string json_string_field(const std::string & msg, const std::string & key) {
    const std::string needle = '"' + key + '"';
    size_t at = msg.find(needle);
    if (at == std::string::npos) return "";
    at = msg.find(':', at + needle.size());
    if (at == std::string::npos) return "";
    at = msg.find('"', at + 1);
    if (at == std::string::npos) return "";
    std::string out;
    for (size_t i = at + 1; i < msg.size(); ++i) {
        const char c = msg[i];
        if (c == '"') return out;
        if (c == '\\' && i + 1 < msg.size()) {
            const char e = msg[++i];
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        } else {
            out += c;
        }
    }
    return "";
}

namespace {

class Parser {
public:
    explicit Parser(const std::string & text) : text_(text) {}

    bool parse(Json & out, std::string & error) {
        if (!value(out, 0)) {
            error = error_ + " at byte " + std::to_string(at_);
            return false;
        }
        space();
        if (at_ != text_.size()) {
            error = "text after the JSON value at byte " + std::to_string(at_);
            return false;
        }
        return true;
    }

private:
    bool fail(const char * message) {
        error_ = message;
        return false;
    }

    void space() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r'))
            ++at_;
    }

    bool literal(const char * word) {
        const std::string w = word;
        if (text_.compare(at_, w.size(), w) != 0) return fail("unexpected character");
        at_ += w.size();
        return true;
    }

    bool value(Json & out, int depth) {
        if (depth > 64) return fail("nested too deeply");
        space();
        if (at_ >= text_.size()) return fail("unexpected end");
        const char c = text_[at_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            std::string s;
            if (!string(s)) return false;
            out = Json(std::move(s));
            return true;
        }
        if (c == 't') { out = Json(true); return literal("true"); }
        if (c == 'f') { out = Json(false); return literal("false"); }
        if (c == 'n') { out = Json(); return literal("null"); }
        return number(out);
    }

    bool object(Json & out, int depth) {
        out = Json::object();
        ++at_;
        space();
        if (at_ < text_.size() && text_[at_] == '}') { ++at_; return true; }
        while (true) {
            space();
            std::string key;
            if (at_ >= text_.size() || text_[at_] != '"') return fail("expected a member name");
            if (!string(key)) return false;
            space();
            if (at_ >= text_.size() || text_[at_] != ':') return fail("expected ':'");
            ++at_;
            Json member;
            if (!value(member, depth + 1)) return false;
            out.set(key, std::move(member));
            space();
            if (at_ < text_.size() && text_[at_] == ',') { ++at_; continue; }
            if (at_ < text_.size() && text_[at_] == '}') { ++at_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Json & out, int depth) {
        out = Json::array();
        ++at_;
        space();
        if (at_ < text_.size() && text_[at_] == ']') { ++at_; return true; }
        while (true) {
            Json item;
            if (!value(item, depth + 1)) return false;
            out.push(std::move(item));
            space();
            if (at_ < text_.size() && text_[at_] == ',') { ++at_; continue; }
            if (at_ < text_.size() && text_[at_] == ']') { ++at_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    static void utf8(uint32_t c, std::string & out) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }

    bool hex4(uint32_t & out) {
        if (at_ + 4 > text_.size()) return fail("short \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[at_++];
            out <<= 4;
            if (h >= '0' && h <= '9') out |= h - '0';
            else if (h >= 'a' && h <= 'f') out |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') out |= h - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool string(std::string & out) {
        ++at_;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size()) break;
            const char e = text_[at_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t c1 = 0;
                    if (!hex4(c1)) return false;
                    if (c1 >= 0xD800 && c1 < 0xDC00 && text_.compare(at_, 2, "\\u") == 0) {
                        at_ += 2;
                        uint32_t c2 = 0;
                        if (!hex4(c2)) return false;
                        c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    }
                    utf8(c1, out);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool number(Json & out) {
        const char * start = text_.c_str() + at_;
        char * end = nullptr;
        const double value = std::strtod(start, &end);
        if (end == start) return fail("unexpected character");
        at_ += static_cast<size_t>(end - start);
        out = Json(value);
        return true;
    }

    const std::string & text_;
    size_t at_ = 0;
    std::string error_;
};

}  // namespace

Json::Json(const std::vector<std::string> & values) : type_(Type::array) {
    for (const std::string & value : values) items_.emplace_back(value);
}

bool Json::parse(const std::string & text, Json & out, std::string & error) {
    return Parser(text).parse(out, error);
}

const std::string & Json::as_string() const {
    static const std::string empty;
    return is_string() ? string_ : empty;
}

std::vector<std::string> Json::as_strings() const {
    std::vector<std::string> out;
    for (const Json & item : items_)
        if (item.is_string()) out.push_back(item.string_);
    return out;
}

const Json & Json::operator[](const std::string & key) const {
    static const Json null;
    for (const auto & [name, value] : members_)
        if (name == key) return value;
    return null;
}

bool Json::has(const std::string & key) const {
    for (const auto & member : members_)
        if (member.first == key) return true;
    return false;
}

Json & Json::set(const std::string & key, Json value) {
    type_ = Type::object;
    for (auto & [name, existing] : members_) {
        if (name == key) {
            existing = std::move(value);
            return *this;
        }
    }
    members_.emplace_back(key, std::move(value));
    return *this;
}

Json & Json::push(Json value) {
    type_ = Type::array;
    items_.push_back(std::move(value));
    return *this;
}

std::string Json::dump() const {
    std::string out;
    dump(out);
    return out;
}

void Json::dump(std::string & out) const {
    switch (type_) {
        case Type::null: out += "null"; break;
        case Type::boolean: out += bool_ ? "true" : "false"; break;
        case Type::number: {
            char buf[32];
            if (std::isfinite(number_) && number_ == std::floor(number_) && std::fabs(number_) < 1e15)
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(number_));
            else if (std::isfinite(number_))
                std::snprintf(buf, sizeof buf, "%.6g", number_);
            else
                std::snprintf(buf, sizeof buf, "null");
            out += buf;
            break;
        }
        case Type::string: out += json_quote(string_); break;
        case Type::array:
            out += '[';
            for (size_t i = 0; i < items_.size(); ++i) {
                if (i) out += ',';
                items_[i].dump(out);
            }
            out += ']';
            break;
        case Type::object:
            out += '{';
            for (size_t i = 0; i < members_.size(); ++i) {
                if (i) out += ',';
                out += json_quote(members_[i].first);
                out += ':';
                members_[i].second.dump(out);
            }
            out += '}';
            break;
    }
}

}  // namespace asrd
