#include "json.h"

#include <cstdio>
#include <cstdlib>

namespace {

void skip_space(const std::string &s, size_t *i)
{
    while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' ||
                             s[*i] == '\r'))
        (*i)++;
}

bool parse_string(const std::string &s, size_t *i, std::string *out)
{
    if (*i >= s.size() || s[*i] != '"')
        return false;
    (*i)++;
    out->clear();
    while (*i < s.size()) {
        char c = s[*i];
        if (c == '"') {
            (*i)++;
            return true;
        }
        if (c == '\\') {
            (*i)++;
            if (*i >= s.size())
                return false;
            char e = s[*i];
            switch (e) {
            case 'n': *out += '\n'; break;
            case 't': *out += '\t'; break;
            case 'r': *out += '\r'; break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case 'u': {
                // Only the BMP subset the protocol can actually produce; a
                // surrogate pair is passed through as replacement characters
                // rather than mis-decoded.
                if (*i + 4 >= s.size())
                    return false;
                unsigned code = 0;
                for (int k = 1; k <= 4; k++) {
                    char h = s[*i + k];
                    code <<= 4;
                    if (h >= '0' && h <= '9')      code |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                *i += 4;
                if (code < 0x80) {
                    *out += (char)code;
                } else if (code < 0x800) {
                    *out += (char)(0xC0 | (code >> 6));
                    *out += (char)(0x80 | (code & 0x3F));
                } else {
                    *out += (char)(0xE0 | (code >> 12));
                    *out += (char)(0x80 | ((code >> 6) & 0x3F));
                    *out += (char)(0x80 | (code & 0x3F));
                }
                break;
            }
            default: *out += e; break;
            }
            (*i)++;
            continue;
        }
        *out += c;
        (*i)++;
    }
    return false;
}

// Advances past a value we don't need to keep (a nested object or array).
bool skip_value(const std::string &s, size_t *i)
{
    skip_space(s, i);
    if (*i >= s.size())
        return false;
    char c = s[*i];
    if (c == '"') {
        std::string ignored;
        return parse_string(s, i, &ignored);
    }
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (*i < s.size()) {
            char d = s[*i];
            if (d == '"') {
                std::string ignored;
                if (!parse_string(s, i, &ignored))
                    return false;
                continue;
            }
            if (d == open)
                depth++;
            else if (d == close) {
                depth--;
                (*i)++;
                if (depth == 0)
                    return true;
                continue;
            }
            (*i)++;
        }
        return false;
    }
    while (*i < s.size() && s[*i] != ',' && s[*i] != '}' && s[*i] != ']')
        (*i)++;
    return true;
}

}  // namespace

bool JsonObject::parse(const std::string &text, JsonObject *out)
{
    out->values_.clear();

    size_t i = 0;
    skip_space(text, &i);
    if (i >= text.size() || text[i] != '{')
        return false;
    i++;

    for (;;) {
        skip_space(text, &i);
        if (i < text.size() && text[i] == '}')
            return true;

        std::string key;
        if (!parse_string(text, &i, &key))
            return false;

        skip_space(text, &i);
        if (i >= text.size() || text[i] != ':')
            return false;
        i++;
        skip_space(text, &i);
        if (i >= text.size())
            return false;

        Value v;
        char c = text[i];
        if (c == '"') {
            if (!parse_string(text, &i, &v.text))
                return false;
            v.kind = Value::String;
        } else if (c == 't' || c == 'f') {
            v.kind = Value::Bool;
            v.flag = (c == 't');
            if (!skip_value(text, &i))
                return false;
        } else if (c == 'n') {
            v.kind = Value::Null;
            if (!skip_value(text, &i))
                return false;
        } else if (c == '{' || c == '[') {
            // Nested structures aren't part of the protocol; record the key so
            // has() still reports it, but keep no value.
            v.kind = Value::Null;
            if (!skip_value(text, &i))
                return false;
        } else {
            size_t start = i;
            while (i < text.size() && text[i] != ',' && text[i] != '}' &&
                   text[i] != ' ' && text[i] != '\n' && text[i] != '\r' &&
                   text[i] != '\t')
                i++;
            v.kind   = Value::Number;
            v.text   = text.substr(start, i - start);
            v.number = atof(v.text.c_str());
        }
        out->values_[key] = v;

        skip_space(text, &i);
        if (i < text.size() && text[i] == ',') {
            i++;
            continue;
        }
        if (i < text.size() && text[i] == '}')
            return true;
        return false;
    }
}

bool JsonObject::has(const std::string &key) const
{
    return values_.find(key) != values_.end();
}

std::string JsonObject::str(const std::string &key, const std::string &fallback) const
{
    auto it = values_.find(key);
    if (it == values_.end() || it->second.kind != Value::String)
        return fallback;
    return it->second.text;
}

double JsonObject::num(const std::string &key, double fallback) const
{
    auto it = values_.find(key);
    if (it == values_.end())
        return fallback;
    if (it->second.kind == Value::Number)
        return it->second.number;
    if (it->second.kind == Value::Bool)
        return it->second.flag ? 1.0 : 0.0;
    // A number that arrived quoted still counts — JavaScript is loose about
    // this and the plugin shouldn't have to be careful.
    if (it->second.kind == Value::String && !it->second.text.empty())
        return atof(it->second.text.c_str());
    return fallback;
}

int JsonObject::integer(const std::string &key, int fallback) const
{
    auto it = values_.find(key);
    if (it == values_.end())
        return fallback;
    return (int)num(key, (double)fallback);
}

bool JsonObject::boolean(const std::string &key, bool fallback) const
{
    auto it = values_.find(key);
    if (it == values_.end())
        return fallback;
    if (it->second.kind == Value::Bool)
        return it->second.flag;
    if (it->second.kind == Value::Number)
        return it->second.number != 0.0;
    if (it->second.kind == Value::String)
        return it->second.text == "true" || it->second.text == "1";
    return fallback;
}

std::string json_escape_string(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\"";  break;
        case '\\': out += "\\\\";  break;
        case '\n': out += "\\n";   break;
        case '\r': out += "\\r";   break;
        case '\t': out += "\\t";   break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}
