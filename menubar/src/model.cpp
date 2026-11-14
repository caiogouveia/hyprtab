#include "model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <format>
#include <string>

namespace mb {

std::string stripMnemonic(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '_') {
            if (i + 1 < s.size() && s[i + 1] == '_') {
                out += '_';
                i++;
            }
            continue;
        }
        out += s[i];
    }
    return out;
}

namespace {

std::string keyName(const std::string& k) {
    if (k == "Control")
        return "Ctrl";
    if (k == "Super")
        return "Super";
    if (k == "plus")
        return "+";
    if (k == "minus")
        return "-";
    if (k == "equal")
        return "=";
    if (k == "comma")
        return ",";
    if (k == "period")
        return ".";
    if (k == "slash")
        return "/";
    if (k == "backslash")
        return "\\";
    if (k == "Delete")
        return "Del";
    if (k == "BackSpace")
        return "Backspace";
    if (k == "Return")
        return "Enter";
    if (k == "Escape")
        return "Esc";
    if (k == "Page_Up" || k == "Prior")
        return "PgUp";
    if (k == "Page_Down" || k == "Next")
        return "PgDn";
    if (k.size() == 1)
        return std::string(1, (char)std::toupper((unsigned char)k[0]));
    return k;
}

} // namespace

std::string formatShortcut(const std::vector<std::vector<std::string>>& chords) {
    std::string out;
    for (auto& chord : chords) {
        if (chord.empty())
            continue;
        if (!out.empty())
            out += ", ";
        for (size_t i = 0; i < chord.size(); i++) {
            if (i)
                out += '+';
            out += keyName(chord[i]);
        }
    }
    return out;
}

const Node* at(const Node& root, const std::vector<size_t>& path) {
    const Node* n = &root;
    for (auto i : path) {
        if (i >= n->children.size())
            return nullptr;
        n = &n->children[i];
    }
    return n;
}

bool selectable(const Node& n) {
    return !n.separator && !n.header && n.enabled;
}

int nextSelectable(const Node& menu, int from, int dir) {
    const int n = (int)menu.children.size();
    if (n == 0)
        return -1;
    int i = from;
    for (int k = 0; k < n; k++) {
        i = from < 0 && k == 0 ? (dir > 0 ? 0 : n - 1) : ((i + dir) % n + n) % n;
        if (selectable(menu.children[i]))
            return i;
    }
    return -1;
}

std::string stripFieldCodes(const std::string& exec) {
    std::string out;
    for (size_t i = 0; i < exec.size(); i++) {
        if (exec[i] == '%' && i + 1 < exec.size()) {
            if (exec[i + 1] == '%')
                out += '%';
            i++;
            continue;
        }
        out += exec[i];
    }
    // espaços sobrando no fim (de "%U" removido)
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// ---------- JSON ----------

const Json* Json::get(const std::string& key) const {
    if (type != Type::Object)
        return nullptr;
    for (auto& [k, v] : object)
        if (k == key)
            return &v;
    return nullptr;
}

namespace {

struct JsonParser {
    const std::string& s;
    size_t             i = 0;
    std::string        err;

    int line() const {
        return 1 + (int)std::count(s.begin(), s.begin() + (long)std::min(i, s.size()), '\n');
    }
    bool fail(const std::string& msg) {
        if (err.empty())
            err = std::format("linha {}: {}", line(), msg);
        return false;
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
    }
    bool lit(const char* word) {
        const size_t n = std::char_traits<char>::length(word);
        if (s.compare(i, n, word) != 0)
            return false;
        i += n;
        return true;
    }
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80)
            out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& v) {
        if (i + 4 > s.size())
            return fail("\\u incompleto");
        v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= (uint32_t)(c - 'A' + 10);
            else
                return fail("\\u inválido");
        }
        return true;
    }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"')
            return fail("esperava texto entre aspas");
        i++;
        while (i < s.size() && s[i] != '"') {
            const char c = s[i++];
            if ((unsigned char)c < 0x20)
                return fail("caractere de controle dentro do texto");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size())
                break;
            const char e = s[i++];
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
                    uint32_t cp = 0;
                    if (!hex4(cp))
                        return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && lit("\\u")) { // par substituto
                        uint32_t lo = 0;
                        if (!hex4(lo))
                            return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: return fail("escape inválido");
            }
        }
        if (i >= s.size())
            return fail("texto sem aspas de fechamento");
        i++;
        return true;
    }
    bool value(Json& v, int depth) {
        if (depth > 64)
            return fail("aninhado demais");
        ws();
        if (i >= s.size())
            return fail("fim inesperado");
        const char c = s[i];
        if (c == '{') {
            v.type = Json::Type::Object;
            i++;
            ws();
            if (i < s.size() && s[i] == '}') {
                i++;
                return true;
            }
            while (true) {
                ws();
                std::string key;
                if (!str(key))
                    return false;
                ws();
                if (i >= s.size() || s[i] != ':')
                    return fail("esperava ':'");
                i++;
                Json child;
                if (!value(child, depth + 1))
                    return false;
                v.object.emplace_back(std::move(key), std::move(child));
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    i++;
                    return true;
                }
                return fail("esperava ',' ou '}'");
            }
        }
        if (c == '[') {
            v.type = Json::Type::Array;
            i++;
            ws();
            if (i < s.size() && s[i] == ']') {
                i++;
                return true;
            }
            while (true) {
                Json child;
                if (!value(child, depth + 1))
                    return false;
                v.array.push_back(std::move(child));
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    i++;
                    return true;
                }
                return fail("esperava ',' ou ']'");
            }
        }
        if (c == '"') {
            v.type = Json::Type::String;
            return str(v.string);
        }
        if (lit("true")) {
            v.type    = Json::Type::Bool;
            v.boolean = true;
            return true;
        }
        if (lit("false")) {
            v.type = Json::Type::Bool;
            return true;
        }
        if (lit("null"))
            return true;
        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = i;
            while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
                i++;
            try {
                v.number = std::stod(s.substr(start, i - start));
            } catch (...) {
                return fail("número inválido");
            }
            v.type = Json::Type::Number;
            return true;
        }
        return fail(std::format("valor inesperado '{}'", c));
    }
};

std::string lower(std::string s) {
    for (auto& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

// tecla como escrita no mapa -> nome de keysym do `sendshortcut`; vazio se desconhecida
std::string keysym(const std::string& k) {
    static const std::pair<const char*, const char*> NAMES[] = {
        {",", "comma"},       {".", "period"},       {"/", "slash"},     {"\\", "backslash"}, {";", "semicolon"},  {"'", "apostrophe"},
        {"`", "grave"},       {"-", "minus"},        {"=", "equal"},     {"+", "plus"},       {"[", "bracketleft"}, {"]", "bracketright"},
        {"enter", "Return"},  {"return", "Return"},  {"esc", "Escape"},  {"escape", "Escape"}, {"del", "Delete"},   {"delete", "Delete"},
        {"backspace", "BackSpace"}, {"space", "space"}, {"tab", "Tab"},  {"pgup", "Prior"},   {"pageup", "Prior"},  {"pgdn", "Next"},
        {"pagedown", "Next"}, {"ins", "Insert"},     {"insert", "Insert"}, {"home", "Home"},  {"end", "End"},       {"up", "Up"},
        {"down", "Down"},     {"left", "Left"},      {"right", "Right"},
    };
    if (k.empty())
        return "";
    if (k.starts_with("code:") && k.size() > 5 && std::all_of(k.begin() + 5, k.end(), [](char c) { return std::isdigit((unsigned char)c); }))
        return k;
    if (k.size() == 1 && std::isalnum((unsigned char)k[0]))
        return std::string(1, (char)std::tolower((unsigned char)k[0]));
    const auto l = lower(k);
    for (auto& [from, to] : NAMES)
        if (l == from)
            return to;
    // F1..F24
    if (l.size() >= 2 && l[0] == 'f' && std::all_of(l.begin() + 1, l.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
        const int n = std::stoi(l.substr(1));
        if (n >= 1 && n <= 24)
            return "F" + std::to_string(n);
    }
    return "";
}

Node itemFromJson(const Json& j, int& nextId, std::string& err);

bool itemsFromJson(const Json* arr, Node& parent, int& nextId, std::string& err) {
    if (!arr || arr->type != Json::Type::Array) {
        err = std::format("\"{}\": falta a lista \"items\"", parent.label);
        return false;
    }
    for (auto& j : arr->array) {
        Node n = itemFromJson(j, nextId, err);
        if (!err.empty())
            return false;
        parent.children.push_back(std::move(n));
    }
    parent.submenu = true;
    return true;
}

Node itemFromJson(const Json& j, int& nextId, std::string& err) {
    Node n;
    if (j.type == Json::Type::String && j.string == "-") {
        n.separator = true;
        return n;
    }
    const Json* label = j.get("label");
    if (!label || label->type != Json::Type::String) {
        err = "item sem \"label\"";
        return n;
    }
    n.label = label->string;
    n.id    = nextId++;
    if (const Json* items = j.get("items")) {
        itemsFromJson(items, n, nextId, err);
        return n;
    }
    const Json* keys = j.get("keys");
    const Json* send = j.get("send");
    if (keys && keys->type == Json::Type::String)
        n.shortcut = keys->string;
    n.keys = send && send->type == Json::Type::String ? send->string : n.shortcut;
    if (!n.keys.empty() && parseKeys(n.keys).empty()) {
        err = std::format("\"{}\": atalho não reconhecido \"{}\"", n.label, n.keys);
        return n;
    }
    n.enabled = !n.keys.empty();
    return n;
}

} // namespace

bool parseJson(const std::string& text, Json& out, std::string& err) {
    JsonParser p{text, 0, {}};
    out = {};
    if (!p.value(out, 0)) {
        err = p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        p.fail("sobra texto depois do fim");
        err = p.err;
        return false;
    }
    return true;
}

std::vector<Chord> parseKeys(const std::string& keys) {
    std::vector<Chord> out;
    size_t             i = 0;
    while (i < keys.size()) {
        while (i < keys.size() && keys[i] == ' ')
            i++;
        size_t j = i;
        while (j < keys.size() && keys[j] != ' ')
            j++;
        if (j == i)
            break;
        std::string chord = keys.substr(i, j - i);
        i                 = j;

        // separa por '+', mas um '+' no fim ("Ctrl++") é a tecla
        std::vector<std::string> parts;
        std::string              cur;
        for (size_t k = 0; k < chord.size(); k++) {
            if (chord[k] == '+' && !cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            } else
                cur += chord[k];
        }
        if (!cur.empty())
            parts.push_back(cur);
        if (parts.empty())
            return {};

        Chord c;
        for (size_t k = 0; k + 1 < parts.size(); k++) {
            const auto m = lower(parts[k]);
            const char* mod = m == "ctrl" || m == "control" ? "CTRL" : m == "shift" ? "SHIFT" : m == "alt" ? "ALT" : m == "super" || m == "meta" ? "SUPER" : nullptr;
            if (!mod)
                return {};
            if (!c.mods.empty())
                c.mods += ' ';
            c.mods += mod;
        }
        c.key = keysym(parts.back());
        if (c.key.empty())
            return {};
        out.push_back(std::move(c));
    }
    return out;
}

bool parseAppMap(const std::string& text, AppMap& out, std::string& err) {
    Json j;
    if (!parseJson(text, j, err))
        return false;
    if (j.type != Json::Type::Object) {
        err = "o arquivo deve ser um objeto { ... }";
        return false;
    }
    out = {};
    if (const Json* n = j.get("name"); n && n->type == Json::Type::String)
        out.name = n->string;
    const Json* match = j.get("match");
    if (!match || match->type != Json::Type::Array || match->array.empty()) {
        err = "falta a lista \"match\" (classes da janela)";
        return false;
    }
    for (auto& m : match->array)
        if (m.type == Json::Type::String && !m.string.empty())
            out.match.push_back(lower(m.string));
    const Json* menus = j.get("menus");
    if (!menus || menus->type != Json::Type::Array) {
        err = "falta a lista \"menus\"";
        return false;
    }
    int nextId = 1;
    for (auto& m : menus->array) {
        Node top = itemFromJson(m, nextId, err);
        if (!err.empty())
            return false;
        if (top.separator || top.children.empty()) {
            err = std::format("menu \"{}\" sem itens", top.label);
            return false;
        }
        out.menus.children.push_back(std::move(top));
    }
    return true;
}

const AppMap* findAppMap(const std::vector<AppMap>& maps, const std::vector<std::string>& classes) {
    for (auto& m : maps)
        for (auto& c : classes)
            if (!c.empty() && std::find(m.match.begin(), m.match.end(), lower(c)) != m.match.end())
                return &m;
    return nullptr;
}

// ---------- geometria ----------

std::vector<Title> layoutTitles(const std::vector<double>& textWidths) {
    std::vector<Title> out;
    double             x = BAR_PADX;
    for (size_t i = 0; i < textWidths.size(); i++) {
        const double w = i == 0 ? LOGO_W : textWidths[i] + 2 * TITLE_PADX;
        out.push_back({x, w});
        x += w;
    }
    return out;
}

int hitTitle(const std::vector<Title>& titles, double x) {
    for (size_t i = 0; i < titles.size(); i++)
        if (x >= titles[i].x && x < titles[i].x + titles[i].w)
            return (int)i;
    return -1;
}

std::string ellipsize(const std::string& s, double maxW, bool bold, const Measure& measure) {
    if (measure(s, bold) <= maxW)
        return s;
    std::vector<size_t> cuts; // começo de cada caractere
    for (size_t i = 1; i < s.size(); i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            cuts.push_back(i);
    // maior prefixo que, com "…", cabe (busca binária)
    size_t lo = 0, hi = cuts.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measure(s.substr(0, cuts[mid - 1]) + "…", bold) <= maxW)
            lo = mid;
        else
            hi = mid - 1;
    }
    auto out = lo ? s.substr(0, cuts[lo - 1]) : std::string();
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out + "…";
}

double dropdownWidth(const Node& menu, const Measure& measure) {
    double label = 0, shortcut = 0, min = MENU_MIN_W;
    bool   arrow = false;
    for (auto& c : menu.children) {
        if (c.separator)
            continue;
        if (c.slider)
            min = std::max(min, SLIDER_MIN_W);
        label = std::max(label, std::min(menu.maxLabelW > 0 ? menu.maxLabelW : LABEL_MAX_W, measure(c.label, c.header)));
        if (!c.shortcut.empty())
            shortcut = std::max(shortcut, measure(c.shortcut, false));
        arrow = arrow || c.submenu;
    }
    const double w = 2 * MENU_PADX + 2 * ROW_PADX + CHECK_W + label + (shortcut > 0 ? SHORT_GAP + shortcut : 0) + (arrow ? ARROW_W : 0);
    return std::clamp(w, min, MENU_MAX_W);
}

namespace {

double rowH(const Node& c) {
    return c.separator ? SEP_H : c.slider ? SLIDER_H : ROW_H;
}

Dropdown place(const Node& menu, const Measure& measure, double x, double y, double monH) {
    Dropdown d;
    d.box.w = dropdownWidth(menu, measure);
    double h = MENU_PADY;
    for (auto& c : menu.children)
        h += rowH(c);
    h += MENU_PADY;
    d.box.h = h;
    d.box.x = x;
    // não passa do fim da tela; se o menu for mais alto que a tela, ao menos o começo aparece
    d.box.y = std::max(0.0, std::min(y, monH - SCREEN_MARGIN - h));
    double ry = d.box.y + MENU_PADY;
    for (auto& c : menu.children) {
        const double rh = rowH(c);
        d.rows.push_back({d.box.x + MENU_PADX, ry, d.box.w - 2 * MENU_PADX, rh});
        ry += rh;
    }
    return d;
}

} // namespace

Dropdown dropdown(const Node& menu, const Measure& measure, const Title& t, double barH, double monW, double monH) {
    const double w = dropdownWidth(menu, measure);
    const double x = std::max(SCREEN_MARGIN, std::min(t.x, monW - SCREEN_MARGIN - w));
    return place(menu, measure, x, barH, monH);
}

Dropdown submenu(const Node& menu, const Measure& measure, const Dropdown& parent, size_t row, double monW, double monH) {
    const double w   = dropdownWidth(menu, measure);
    const double ry  = row < parent.rows.size() ? parent.rows[row].y : parent.box.y;
    double       x   = parent.box.x + parent.box.w - 2;
    if (x + w > monW - SCREEN_MARGIN)
        x = std::max(SCREEN_MARGIN, parent.box.x - w + 2);
    return place(menu, measure, x, ry - MENU_PADY, monH);
}

int hitRow(const Dropdown& d, double x, double y) {
    if (!d.box.contains(x, y))
        return -2;
    for (size_t i = 0; i < d.rows.size(); i++)
        if (d.rows[i].contains(x, y))
            return (int)i;
    return -1;
}

Box sliderTrack(const Box& row, double pctW) {
    const double x   = row.x + ROW_PADX + SLIDER_KNOB_R;
    const double end = row.x + row.w - ROW_PADX - pctW - SLIDER_GAP - SLIDER_KNOB_R;
    return {x, row.y + row.h / 2 - 2, std::max(1.0, end - x), 4};
}

double sliderValueAt(const Box& row, double pctW, double x) {
    const auto t = sliderTrack(row, pctW);
    return std::clamp((x - t.x) / t.w, 0.0, 1.0);
}

Title volumeTitle(double monW, double clockW) {
    const double right = monW - (clockW > 0 ? CLOCK_PADX + clockW + STATUS_GAP : BAR_PADX);
    return {right - VOLUME_W, VOLUME_W};
}

bool parseVolume(const std::string& text, Volume& out) {
    const auto p = text.find("Volume:");
    if (p == std::string::npos)
        return false;
    const char* s   = text.c_str() + p + 7;
    char*       end = nullptr;
    const double v  = std::strtod(s, &end);
    if (end == s || !std::isfinite(v) || v < 0)
        return false;
    out.vol   = v;
    out.muted = text.find("[MUTED]", p) != std::string::npos;
    return true;
}

namespace {
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t");
    const auto b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
} // namespace

std::vector<Sink> parseSinks(const std::string& status) {
    std::vector<Sink>        out;
    std::vector<std::string> devices;
    bool                     audio = false, sinks = false, devs = false;
    size_t            pos   = 0;
    while (pos <= status.size()) {
        auto eol = status.find('\n', pos);
        if (eol == std::string::npos)
            eol = status.size();
        const std::string line = status.substr(pos, eol - pos);
        pos                    = eol + 1;
        if (!line.empty() && line[0] != ' ' && (unsigned char)line[0] < 0x80) { // seção de nível 0 ("Audio", "Video")
            audio = line.starts_with("Audio");
            sinks = devs = false;
            continue;
        }
        if (!audio)
            continue;
        if (line.find("─ ") != std::string::npos) { // " ├─ Sinks:"
            sinks = line.find("Sinks:") != std::string::npos;
            devs  = line.find("Devices:") != std::string::npos;
            continue;
        }
        if (!sinks && !devs)
            continue;
        // " │  *   51. Nome [vol: 0.40]"
        std::string rest = line;
        if (const auto bar = rest.find("│"); bar != std::string::npos)
            rest = rest.substr(bar + std::string("│").size());
        rest     = trim(rest);
        Sink s;
        if (rest.starts_with("*")) {
            s.def = true;
            rest  = trim(rest.substr(1));
        }
        const auto dot = rest.find(". ");
        if (dot == std::string::npos || dot == 0 || !std::all_of(rest.begin(), rest.begin() + dot, [](char c) { return std::isdigit((unsigned char)c); }))
            continue;
        s.id = std::atoi(rest.substr(0, dot).c_str());
        rest = rest.substr(dot + 2);
        if (devs) { // " │      52. Ryzen HD Audio Controller (PRIME B450M-A Motherboard) [alsa]"
            if (const auto b = rest.rfind(" ["); b != std::string::npos && rest.ends_with("]"))
                rest = rest.substr(0, b);
            if (!trim(rest).empty())
                devices.push_back(trim(rest));
            continue;
        }
        if (const auto v = rest.rfind(" [vol:"); v != std::string::npos)
            rest = rest.substr(0, v);
        s.name = trim(rest);
        out.push_back(s);
    }
    // label: o perfil, sem o dispositivo (o nome de dispositivo mais longo que for prefixo)
    for (auto& s : out) {
        size_t best = 0;
        for (auto& d : devices)
            if (d.size() > best && s.name.size() > d.size() + 1 && s.name.starts_with(d) && s.name[d.size()] == ' ')
                best = d.size();
        s.label = best ? trim(s.name.substr(best)) : shortSinkName(s.name);
    }
    std::vector<bool> dup(out.size());
    for (size_t i = 0; i < out.size(); i++)
        for (size_t j = 0; j < out.size(); j++)
            dup[i] = dup[i] || (i != j && out[i].label == out[j].label);
    for (size_t i = 0; i < out.size(); i++)
        if (dup[i])
            out[i].label = shortSinkName(out[i].name);
    return out;
}

std::string shortSinkName(const std::string& name) {
    std::string out;
    size_t      i = 0;
    while (i < name.size()) {
        if (name[i] == '(') {
            const auto close = name.find(')', i);
            if (close != std::string::npos && close + 1 < name.size()) { // tem texto depois: é o nome do produto
                i = close + 1;
                while (i < name.size() && name[i] == ' ')
                    i++;
                if (!out.empty() && out.back() != ' ')
                    out += ' ';
                continue;
            }
        }
        out += name[i++];
    }
    return trim(out);
}

int volumeLevel(double vol, bool muted) {
    if (muted || vol <= 0.005)
        return 0;
    return vol < 0.34 ? 1 : vol < 0.67 ? 2 : 3;
}

std::string clockText(int wday, int mday, int mon, int hour, int min) {
    static const char* DAYS[]   = {"dom", "seg", "ter", "qua", "qui", "sex", "sáb"};
    static const char* MONTHS[] = {"jan", "fev", "mar", "abr", "mai", "jun", "jul", "ago", "set", "out", "nov", "dez"};
    return std::format("{} {} {}  {:02}:{:02}", DAYS[((wday % 7) + 7) % 7], mday, MONTHS[((mon % 12) + 12) % 12], hour, min);
}

} // namespace mb
