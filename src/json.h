// json.h - a ~200 line JSON reader, enough to parse scene.json without pulling a dependency
// into a single-executable build. Supports objects, arrays, strings, numbers, true/false/null.
#ifndef JSON_H
#define JSON_H

#include <map>
#include <memory>
#include <string>
#include <vector>

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    bool has(const char* key) const;
    const JsonValue* find(const char* key) const;
    double num(const char* key, double fallback) const;
    bool booleanAt(const char* key, bool fallback) const;
    std::string text(const char* key, const std::string& fallback) const;

    // Reads a numeric array of exactly `n` components ([r,g,b] or [x,y,z,w]); returns false if the
    // key is absent or has fewer components than requested (remaining slots keep `fallback`).
    bool vecN(const char* key, float* out, int n, float fallback = 0.0f) const;
};

// Parses `text`. On failure returns false and fills `error` with a line/column hint.
bool jsonParse(const std::string& text, JsonValue* out, std::string* error);

#endif  // JSON_H
