/*
 * Just enough JSON for the control protocol.
 *
 * Every message the plugin sends is a flat object of strings, numbers and
 * booleans, so this parses exactly that and skips over anything nested rather
 * than pulling in a dependency for a handful of fields.  Unknown keys are kept
 * — the plugin and the helper are versioned separately and each has to
 * tolerate fields the other doesn't know about.
 */

#ifndef IINA_DECKLINK_JSON_H
#define IINA_DECKLINK_JSON_H

#include <map>
#include <string>

class JsonObject {
public:
    // Returns false if the text isn't a JSON object at all.
    static bool parse(const std::string &text, JsonObject *out);

    bool has(const std::string &key) const;
    std::string str(const std::string &key, const std::string &fallback = "") const;
    double num(const std::string &key, double fallback = 0.0) const;
    int    integer(const std::string &key, int fallback = 0) const;
    bool   boolean(const std::string &key, bool fallback = false) const;

private:
    // Values are kept in their source form and converted on access, which
    // keeps the parser trivial and costs nothing at these message rates.
    struct Value {
        enum Kind { String, Number, Bool, Null } kind = Null;
        std::string text;
        double      number = 0.0;
        bool        flag   = false;
    };
    std::map<std::string, Value> values_;
};

// Escapes a string for inclusion in JSON output (without surrounding quotes).
std::string json_escape_string(const std::string &s);

#endif  // IINA_DECKLINK_JSON_H
