// The little JSON asrd needs: a value to build replies and manifests with and to read request bodies into, and the
// quoting the dictation's events use.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace asrd {

std::string json_quote(const std::string & s);

// The value of a top-level string field such as "type" in {"type":"stop"}; "" when absent. Control messages are
// small and flat, so this skips a full parser: it finds "key", a colon, and reads the quoted string after it.
std::string json_string_field(const std::string & msg, const std::string & key);

// A JSON value. Objects keep their members in the order they were set, so replies read the way they are built.
class Json {
public:
    enum class Type { null, boolean, number, string, array, object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::boolean), bool_(value) {}
    Json(int value) : type_(Type::number), number_(value) {}
    Json(int64_t value) : type_(Type::number), number_(static_cast<double>(value)) {}
    Json(uint64_t value) : type_(Type::number), number_(static_cast<double>(value)) {}
    Json(double value) : type_(Type::number), number_(value) {}
    Json(const char * value) : type_(Type::string), string_(value) {}
    Json(std::string value) : type_(Type::string), string_(std::move(value)) {}
    Json(const std::vector<std::string> & values);

    static Json array() { Json j; j.type_ = Type::array; return j; }
    static Json object() { Json j; j.type_ = Type::object; return j; }
    // False with `error` set when `text` is not one JSON value.
    static bool parse(const std::string & text, Json & out, std::string & error);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::null; }
    bool is_bool() const { return type_ == Type::boolean; }
    bool is_number() const { return type_ == Type::number; }
    bool is_string() const { return type_ == Type::string; }
    bool is_array() const { return type_ == Type::array; }
    bool is_object() const { return type_ == Type::object; }

    bool as_bool(bool fallback = false) const { return is_bool() ? bool_ : fallback; }
    double as_number(double fallback = 0) const { return is_number() ? number_ : fallback; }
    int64_t as_int(int64_t fallback = 0) const { return is_number() ? static_cast<int64_t>(number_) : fallback; }
    const std::string & as_string() const;
    std::vector<std::string> as_strings() const;

    // A member of an object, or null when absent or not an object.
    const Json & operator[](const std::string & key) const;
    bool has(const std::string & key) const;
    Json & set(const std::string & key, Json value);
    Json & push(Json value);

    const std::vector<Json> & items() const { return items_; }
    const std::vector<std::pair<std::string, Json>> & members() const { return members_; }

    std::string dump() const;

private:
    void dump(std::string & out) const;

    Type type_ = Type::null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

}  // namespace asrd
