// json.cpp - see json.h
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

struct Parser {
    const char* p;
    const char* end;
    size_t line = 1;

    void skipWs() {
        while (p < end) {
            char c = *p;
            if (c == '\n') { ++line; ++p; }
            else if (c == ' ' || c == '\t' || c == '\r' || c == ':') { ++p; }
            else if (c == '/' && p + 1 < end && p[1] == '/') {  // line comment (tolerated)
                while (p < end && *p != '\n') ++p;
            } else {
                break;
            }
        }
    }
    bool fail(std::string* err, const char* msg) {
        if (err) {
            char buf[160];
            snprintf(buf, sizeof(buf), "line %zu: %s", line, msg);
            *err = buf;
        }
        return false;
    }

    bool parseValue(JsonValue* out, std::string* err);
};

bool readStringRaw(const char** pp, const char* end, std::string* out, std::string* err) {
    const char* p = *pp;
    if (p >= end || *p != '"') { if (err) *err = "expected string"; return false; }
    ++p;
    out->clear();
    while (p < end && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (p >= end) { if (err) *err = "bad escape"; return false; }
            switch (*p) {
                case 'n': out->push_back('\n'); break;
                case 't': out->push_back('\t'); break;
                case 'r': out->push_back('\r'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'u': {
                    if (p + 4 >= end) { if (err) *err = "bad \\u"; return false; }
                    int cp = (int)strtol(std::string(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (cp < 0x80) out->push_back(char(cp));
                    else if (cp < 0x800) {
                        out->push_back(char(0xC0 | (cp >> 6)));
                        out->push_back(char(0x80 | (cp & 0x3F)));
                    } else {
                        out->push_back(char(0xE0 | (cp >> 12)));
                        out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                        out->push_back(char(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: out->push_back(*p); break;
            }
            ++p;
        } else {
            out->push_back(*p++);
        }
    }
    if (p >= end) { if (err) *err = "unterminated string"; return false; }
    *pp = p + 1;  // past closing quote
    return true;
}

bool Parser::parseValue(JsonValue* out, std::string* err) {
    skipWs();
    if (p >= end) return fail(err, "unexpected end of input");
    char c = *p;
    if (c == '{') {
        ++p;
        out->type = JsonValue::Type::Object;
        for (;;) {
            skipWs();
            if (p >= end) return fail(err, "unterminated object");
            if (*p == '}') { ++p; return true; }
            std::string key;
            if (!readStringRaw(&p, end, &key, err)) return fail(err, "expected object key");
            skipWs();
            if (p < end && *p == ',') ++p;
            skipWs();
            if (p >= end) return fail(err, "missing value for key");
            if (*p == ':') ++p;
            JsonValue v;
            if (!parseValue(&v, err)) return false;
            out->object.emplace_back(key, std::move(v));
            skipWs();
            if (p < end && *p == ',') ++p;
        }
    }
    if (c == '[') {
        ++p;
        out->type = JsonValue::Type::Array;
        for (;;) {
            skipWs();
            if (p >= end) return fail(err, "unterminated array");
            if (*p == ']') { ++p; return true; }
            if (*p == ',') { ++p; continue; }
            JsonValue v;
            if (!parseValue(&v, err)) return false;
            out->array.push_back(std::move(v));
        }
    }
    if (c == '"') {
        out->type = JsonValue::Type::String;
        return readStringRaw(&p, end, &out->str, err);
    }
    if (end - p >= 4 && strncmp(p, "true", 4) == 0) { p += 4; out->type = JsonValue::Type::Bool; out->boolean = true; return true; }
    if (end - p >= 5 && strncmp(p, "false", 5) == 0) { p += 5; out->type = JsonValue::Type::Bool; out->boolean = false; return true; }
    if (end - p >= 4 && strncmp(p, "null", 4) == 0) { p += 4; out->type = JsonValue::Type::Null; return true; }
    {
        char* stop = nullptr;
        double d = strtod(p, &stop);
        if (stop == p) return fail(err, "unexpected token");
        p = stop;
        out->type = JsonValue::Type::Number;
        out->number = d;
        return true;
    }
}

}  // namespace

bool JsonValue::has(const char* key) const { return find(key) != nullptr; }

const JsonValue* JsonValue::find(const char* key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : object)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

double JsonValue::num(const char* key, double fallback) const {
    const JsonValue* v = find(key);
    if (!v) return fallback;
    if (v->type == Type::Number) return v->number;
    if (v->type == Type::Bool) return v->boolean ? 1.0 : 0.0;
    return fallback;
}

bool JsonValue::booleanAt(const char* key, bool fallback) const {
    const JsonValue* v = find(key);
    if (!v) return fallback;
    if (v->type == Type::Bool) return v->boolean;
    if (v->type == Type::Number) return v->number != 0.0;
    return fallback;
}

std::string JsonValue::text(const char* key, const std::string& fallback) const {
    const JsonValue* v = find(key);
    return (v && v->type == Type::String) ? v->str : fallback;
}

bool JsonValue::vecN(const char* key, float* out, int n, float fallback) const {
    const JsonValue* v = find(key);
    if (!v) return false;
    if (v->type == Type::Array) {
        for (int i = 0; i < n; ++i) out[i] = i < (int)v->array.size() ? float(v->array[i].number) : fallback;
        return !v->array.empty();
    }
    if (v->type == Type::Number) {  // a scalar applies to every component
        for (int i = 0; i < n; ++i) out[i] = float(v->number);
        return true;
    }
    return false;
}

bool jsonParse(const std::string& text, JsonValue* out, std::string* error) {
    Parser ps{text.data(), text.data() + text.size(), 1};
    if (!ps.parseValue(out, error)) return false;
    ps.skipWs();
    return true;
}
