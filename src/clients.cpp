// Parser mínimo do JSON de `hyprctl clients -j` (sem dependência do Hyprland, testável).

#include "clients.hpp"

#include <algorithm>
#include <cctype>

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return "";
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

struct Json {
    const std::string& s;
    size_t             i = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r'))
            i++;
    }
    std::string str() { // assume s[i] == '"'
        std::string out;
        i++;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                i++;
                switch (s[i]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': { // \uXXXX: mantém ASCII, troca o resto (e o que não for hex válido)
                        int cp = 0, k = 0;
                        for (; k < 4 && i + 1 < s.size() && std::isxdigit((unsigned char)s[i + 1]); k++, i++)
                            cp = cp * 16 + (std::isdigit((unsigned char)s[i + 1]) ? s[i + 1] - '0' : std::tolower((unsigned char)s[i + 1]) - 'a' + 10);
                        out += (k == 4 && cp < 128) ? (char)cp : '?';
                        break;
                    }
                    default: out += s[i];
                }
            } else
                out += s[i];
            i++;
        }
        i++; // fecha aspas
        return out;
    }
    void skip() { // pula um valor qualquer
        ws();
        if (i >= s.size())
            return;
        if (s[i] == '"') {
            str();
        } else if (s[i] == '{' || s[i] == '[') {
            const char open = s[i], close = open == '{' ? '}' : ']';
            i++;
            ws();
            if (s[i] == close) {
                i++;
                return;
            }
            for (;;) {
                if (open == '{') {
                    ws();
                    str();
                    ws();
                    i++; // ':'
                }
                skip();
                ws();
                if (s[i] == ',') {
                    i++;
                    continue;
                }
                i++; // fecha
                return;
            }
        } else {
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']')
                i++;
        }
    }
};

} // namespace

Clients parseClients(const std::string& js) {
    Clients out;
    Json                                                                p{js};
    p.ws();
    if (p.i >= js.size() || js[p.i] != '[')
        return out;
    p.i++;
    for (;;) {
        p.ws();
        if (p.i >= js.size() || js[p.i] == ']')
            break;
        if (js[p.i] == ',') {
            p.i++;
            continue;
        }
        if (js[p.i] != '{') {
            p.skip();
            continue;
        }
        p.i++;
        std::string a, c, t;
        for (;;) {
            p.ws();
            if (p.i >= js.size() || js[p.i] == '}') {
                p.i++;
                break;
            }
            if (js[p.i] == ',') {
                p.i++;
                continue;
            }
            const auto key = p.str();
            p.ws();
            p.i++; // ':'
            p.ws();
            if (p.i < js.size() && js[p.i] == '"' && (key == "address" || key == "class" || key == "title")) {
                auto v = p.str();
                (key == "address" ? a : key == "class" ? c : t) = v;
            } else
                p.skip();
        }
        if (!a.empty())
            out[a] = {c, t};
    }
    return out;
}

std::vector<std::string> splitList(const std::string& csv) {
    std::vector<std::string> out;
    size_t                   start = 0;
    while (start <= csv.size()) {
        const auto end = std::min(csv.find(',', start), csv.size());
        if (auto v = trim(csv.substr(start, end - start)); !v.empty())
            out.push_back(std::move(v));
        start = end + 1;
    }
    return out;
}

bool classIgnored(const std::string& cls, const std::vector<std::string>& patterns) {
    const auto c = lower(cls);
    for (const auto& raw : patterns) {
        auto p = lower(trim(raw));
        if (p.empty())
            continue;
        const bool head = p.front() == '*', tail = p.size() > 1 && p.back() == '*';
        if (head)
            p.erase(0, 1);
        if (tail)
            p.pop_back();
        if (head && tail ? c.find(p) != std::string::npos : head ? c.ends_with(p) : tail ? c.starts_with(p) : c == p)
            return true;
    }
    return false;
}
