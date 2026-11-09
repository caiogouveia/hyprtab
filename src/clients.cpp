// Parser mínimo do JSON de `hyprctl clients -j` (sem dependência do Hyprland, testável).

#include "clients.hpp"

#include <cctype>

namespace {

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

