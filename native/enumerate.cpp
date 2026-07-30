#include "enumerate.h"

#include <vector>

#include "decklink_shim.h"
#include "json.h"
#include "log.h"

namespace {

std::string quoted(const std::string &s)
{
    return "\"" + json_escape_string(s) + "\"";
}

std::vector<std::string> split_lines(const char *text)
{
    std::vector<std::string> lines;
    if (!text)
        return lines;
    const char *start = text;
    for (const char *p = text;; p++) {
        if (*p == '\n' || *p == '\0') {
            if (p > start)
                lines.emplace_back(start, (size_t)(p - start));
            if (*p == '\0')
                break;
            start = p + 1;
        }
    }
    return lines;
}

std::vector<std::string> split_tabs(const std::string &line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t i = 0;; i++) {
        if (i == line.size() || line[i] == '\t') {
            fields.push_back(line.substr(start, i - start));
            if (i == line.size())
                break;
            start = i + 1;
        }
    }
    return fields;
}

}  // namespace

std::string devices_json()
{
    char *text = dlk_enumerate_devices();
    const bool driver_present = (text != nullptr);
    std::vector<std::string> names = split_lines(text);
    if (text)
        dlk_free_string(text);

    std::string out = "{\"driver\":";
    out += driver_present ? "true" : "false";
    out += ",\"devices\":[";
    for (size_t i = 0; i < names.size(); i++) {
        if (i)
            out += ",";
        out += quoted(names[i]);
    }
    out += "]}";
    return out;
}

std::string modes_json(const std::string &device)
{
    char *text = dlk_enumerate_modes(device.empty() ? nullptr : device.c_str());
    if (!text) {
        log_error("could not enumerate modes for device '%s'",
                  device.empty() ? "(first)" : device.c_str());
        return "{\"modes\":[]}";
    }
    std::vector<std::string> lines = split_lines(text);
    dlk_free_string(text);

    std::string out = "{\"modes\":[";
    bool first = true;
    for (const std::string &line : lines) {
        std::vector<std::string> f = split_tabs(line);  // code w h fps scan
        if (f.size() < 5)
            continue;
        if (!first)
            out += ",";
        first = false;
        out += "{\"code\":" + quoted(f[0]) +
               ",\"width\":" + f[1] +
               ",\"height\":" + f[2] +
               ",\"fps\":" + f[3] +
               ",\"progressive\":" + (f[4] == "p" ? "true" : "false") + "}";
    }
    out += "]}";
    return out;
}
