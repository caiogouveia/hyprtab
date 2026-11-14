#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace hf {

// ---------- texto ----------

namespace {

// letra base de um caractere latino de 2 bytes começando em 0xC3 (Latin-1), ou 0
char latinBase(unsigned char b) {
    b &= ~0x20; // maiúsculas e minúsculas ficam 0x20 de distância nesse bloco
    if (b >= 0x80 && b <= 0x85)
        return 'a';
    if (b == 0x87)
        return 'c';
    if (b >= 0x88 && b <= 0x8B)
        return 'e';
    if (b >= 0x8C && b <= 0x8F)
        return 'i';
    if (b == 0x91)
        return 'n';
    if ((b >= 0x92 && b <= 0x96) || b == 0x98)
        return 'o';
    if (b >= 0x99 && b <= 0x9C)
        return 'u';
    if (b == 0x9D)
        return 'y';
    return 0;
}

bool isAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (unsigned char)c >= 0x80;
}

} // namespace

std::string fold(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = s[i];
        if (c == 0xC3 && i + 1 < s.size()) {
            if (const char b = latinBase((unsigned char)s[i + 1])) {
                out += b;
                i++;
                continue;
            }
        }
        out += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    return out;
}

void popChar(std::string& s) {
    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80)
        s.pop_back();
    if (!s.empty())
        s.pop_back();
}

void popWord(std::string& s) {
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    while (!s.empty() && s.back() != ' ')
        s.pop_back();
}

std::string shellQuote(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::string              cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!cur.empty())
                out.push_back(std::move(cur));
            cur.clear();
        } else
            cur += c;
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

// ---------- busca fuzzy ----------

int fuzzy(std::string_view q, std::string_view t) {
    if (q.empty())
        return 0;
    if (q.size() > t.size())
        return -1;
    const int shortness = -(int)std::min<size_t>(t.size(), 60) / 4; // empate: o texto mais curto ganha

    // trecho contínuo: a melhor ocorrência (começo do texto > começo de palavra > meio)
    int best = -1;
    for (size_t pos = t.find(q); pos != std::string_view::npos; pos = t.find(q, pos + 1)) {
        int s = 200 + 10 * (int)q.size();
        if (pos == 0)
            s += 80;
        else if (!isAlnum(t[pos - 1]))
            s += 50;
        else
            s -= (int)std::min<size_t>(pos, 20);
        best = std::max(best, s);
    }
    if (best >= 0)
        return best + shortness;

    // letras espalhadas, na ordem. Programação dinâmica: cur[j] = melhor pontuação com a letra atual
    // de q em t[j]. Cada letra vale 10; colada na anterior +8; começo do texto +15, de palavra +12;
    // solta (nem colada nem em começo de palavra) só 2; buraco desde a anterior custa 1 por letra
    // pulada, no máximo 3.
    constexpr int NONE = -1000000;
    const size_t  n    = t.size();
    std::vector<int> prev(n, NONE), cur(n, NONE);
    for (size_t i = 0; i < q.size(); i++) {
        int far = NONE; // melhor prev[k] com k <= j-4 (buraco de 3 ou mais)
        for (size_t j = 0; j < n; j++) {
            if (j >= 4)
                far = std::max(far, prev[j - 4]);
            cur[j] = NONE;
            if (t[j] != q[i])
                continue;
            const int bonus = j == 0 ? 25 : !isAlnum(t[j - 1]) ? 22 : 10;
            if (i == 0) {
                cur[j] = bonus == 10 ? 2 : bonus;
                continue;
            }
            int best = NONE;
            const int loose = bonus == 10 ? 2 : bonus; // longe da anterior, fora de começo de palavra
            if (j >= 1 && prev[j - 1] > NONE / 2)
                best = std::max(best, prev[j - 1] + 8 + bonus);
            if (j >= 2 && prev[j - 2] > NONE / 2)
                best = std::max(best, prev[j - 2] - 1 + loose);
            if (j >= 3 && prev[j - 3] > NONE / 2)
                best = std::max(best, prev[j - 3] - 2 + loose);
            if (far > NONE / 2)
                best = std::max(best, far - 3 + loose);
            cur[j] = best;
        }
        std::swap(prev, cur);
    }
    const int s = *std::max_element(prev.begin(), prev.end());
    // espalhado demais (letras soltas pelo meio das palavras): não conta como casamento
    if (s < 11 * (int)q.size())
        return -1;
    return std::min(s, 190) + shortness;
}

// ---------- calculadora ----------

namespace {

struct Parser {
    std::string s;
    size_t      i     = 0;
    int         ops   = 0; // operações de verdade (um número sozinho não é conta)
    bool        error = false;

    void skip() {
        while (i < s.size() && s[i] == ' ')
            i++;
    }
    bool eat(char c) {
        skip();
        if (i < s.size() && s[i] == c) {
            i++;
            return true;
        }
        return false;
    }

    double expr() {
        double v = term();
        while (!error) {
            if (eat('+'))
                v += term();
            else if (eat('-'))
                v -= term();
            else
                break;
            ops++;
        }
        return v;
    }
    double term() {
        double v = unary();
        while (!error) {
            if (eat('*'))
                v *= unary();
            else if (eat('/'))
                v /= unary();
            else
                break;
            ops++;
        }
        return v;
    }
    double unary() {
        if (eat('-'))
            return -unary();
        if (eat('+'))
            return unary();
        return power();
    }
    double power() {
        const double b = postfix();
        if (eat('^')) {
            ops++;
            return std::pow(b, unary());
        }
        return b;
    }
    double postfix() {
        double v = primary();
        while (!error && eat('%')) {
            v /= 100;
            ops++;
        }
        return v;
    }
    double primary() {
        skip();
        if (i >= s.size()) {
            error = true;
            return 0;
        }
        if (eat('(')) {
            const double v = expr();
            if (!eat(')'))
                error = true;
            return v;
        }
        const char c = s[i];
        if ((c >= '0' && c <= '9') || c == '.' || c == ',') {
            std::string num;
            bool        dot = false, digit = false;
            while (i < s.size()) {
                const char d = s[i];
                if (d >= '0' && d <= '9') {
                    digit = true;
                    num += d;
                } else if ((d == '.' || d == ',') && !dot) {
                    dot = true;
                    num += '.';
                } else
                    break;
                i++;
            }
            if (!digit)
                error = true;
            return std::strtod(num.c_str(), nullptr);
        }
        if (c >= 'a' && c <= 'z') {
            std::string id;
            while (i < s.size() && s[i] >= 'a' && s[i] <= 'z')
                id += s[i++];
            if (id == "pi")
                return M_PI;
            if (id == "e")
                return M_E;
            static const std::map<std::string, double (*)(double)> fns = {
                {"sqrt", [](double x) { return std::sqrt(x); }}, {"raiz", [](double x) { return std::sqrt(x); }},
                {"sin", [](double x) { return std::sin(x); }},   {"sen", [](double x) { return std::sin(x); }},
                {"cos", [](double x) { return std::cos(x); }},   {"tan", [](double x) { return std::tan(x); }},
                {"tg", [](double x) { return std::tan(x); }},    {"log", [](double x) { return std::log10(x); }},
                {"ln", [](double x) { return std::log(x); }},    {"abs", [](double x) { return std::fabs(x); }},
            };
            const auto it = fns.find(id);
            if (it == fns.end()) {
                error = true;
                return 0;
            }
            ops++;
            return it->second(power()); // sqrt(9), sqrt 9, sqrt 2^2
        }
        error = true;
        return 0;
    }
};

} // namespace

std::optional<double> calc(std::string_view expr) {
    // normaliza: minúsculas, × ÷ viram * /, "x" entre números multiplica
    std::string s;
    for (size_t i = 0; i < expr.size(); i++) {
        const unsigned char c = expr[i];
        if (c == 0xC3 && i + 1 < expr.size() && (unsigned char)expr[i + 1] == 0x97) { // ×
            s += '*';
            i++;
        } else if (c == 0xC3 && i + 1 < expr.size() && (unsigned char)expr[i + 1] == 0xB7) { // ÷
            s += '/';
            i++;
        } else if (c == 'x' || c == 'X') {
            size_t p = s.find_last_not_of(' ');
            s += (p != std::string::npos && ((s[p] >= '0' && s[p] <= '9') || s[p] == ')')) ? '*' : 'x';
        } else if (c >= 'A' && c <= 'Z')
            s += (char)(c + 32);
        else if (c == '\t')
            s += ' ';
        else
            s += (char)c;
    }
    Parser p{s};
    const double v = p.expr();
    p.skip();
    if (p.error || p.i != p.s.size() || p.ops == 0 || !std::isfinite(v))
        return std::nullopt;
    return v;
}

std::string formatNumber(double v, bool comma) {
    char buf[64];
    if (std::fabs(v) < 1e15 && v == std::round(v))
        std::snprintf(buf, sizeof buf, "%.0f", v == 0 ? 0.0 : v); // sem "-0"
    else
        std::snprintf(buf, sizeof buf, "%.10g", v);
    std::string s = buf;
    const auto  e = s.find('e');
    if (s.find('.') != std::string::npos) { // zeros sobrando na parte decimal (antes do expoente)
        std::string mant = s.substr(0, e), exp = e == std::string::npos ? "" : s.substr(e);
        while (mant.ends_with('0'))
            mant.pop_back();
        if (mant.ends_with('.'))
            mant.pop_back();
        s = mant + exp;
    }
    if (comma)
        std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

bool usesComma(std::string_view expr) {
    return expr.find(',') != std::string_view::npos;
}

// ---------- apps ----------

std::vector<std::string> localeLangs(std::string_view lang) {
    std::string l(lang);
    for (char cut : {'.', '@'})
        if (auto p = l.find(cut); p != std::string::npos)
            l.resize(p);
    if (l.empty() || l == "C" || l == "POSIX")
        return {};
    std::vector<std::string> out = {l};
    if (auto p = l.find('_'); p != std::string::npos)
        out.push_back(l.substr(0, p));
    return out;
}

std::string stripFieldCodes(std::string_view exec) {
    std::string s;
    for (size_t i = 0; i < exec.size(); i++) {
        if (exec[i] == '%' && i + 1 < exec.size()) {
            if (exec[i + 1] == '%')
                s += '%';
            i++;
            continue;
        }
        s += exec[i];
    }
    // argumentos que ficaram vazios ("%u" -> "") e espaços repetidos
    std::string out;
    for (auto& w : words(s)) {
        if (w == "\"\"" || w == "''")
            continue;
        if (!out.empty())
            out += ' ';
        out += w;
    }
    return out;
}

namespace {

bool listHas(std::string_view list, std::string_view desktop) {
    // OnlyShowIn=GNOME;KDE;  XDG_CURRENT_DESKTOP=ubuntu:GNOME
    std::set<std::string> want;
    std::string           cur;
    for (char c : desktop) {
        if (c == ':') {
            want.insert(fold(cur));
            cur.clear();
        } else
            cur += c;
    }
    want.insert(fold(cur));
    cur.clear();
    for (char c : std::string(list) + ";") {
        if (c == ';') {
            if (!cur.empty() && want.contains(fold(cur)))
                return true;
            cur.clear();
        } else
            cur += c;
    }
    return false;
}

std::string semis(std::string s) {
    std::replace(s.begin(), s.end(), ';', ' ');
    return s;
}

} // namespace

std::optional<App> parseDesktop(const std::string& id, const std::string& content, const std::vector<std::string>& langs, std::string_view desktop) {
    std::map<std::string, std::string> kv;
    std::istringstream                 in(content);
    std::string                        line;
    bool                               main = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with('[')) {
            main = line.starts_with("[Desktop Entry]");
            continue;
        }
        if (!main || line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ')
            k.pop_back();
        while (!v.empty() && v.front() == ' ')
            v.erase(0, 1);
        kv.emplace(k, v);
    }
    auto get = [&](const std::string& k) -> std::string {
        auto it = kv.find(k);
        return it == kv.end() ? "" : it->second;
    };
    auto local = [&](const std::string& k) -> std::string {
        for (auto& l : langs)
            if (auto it = kv.find(k + "[" + l + "]"); it != kv.end() && !it->second.empty())
                return it->second;
        return get(k);
    };

    if (get("Type") != "Application" || get("NoDisplay") == "true" || get("Hidden") == "true")
        return std::nullopt;
    if (const auto only = get("OnlyShowIn"); !only.empty() && !listHas(only, desktop))
        return std::nullopt;
    if (const auto not_ = get("NotShowIn"); !not_.empty() && listHas(not_, desktop))
        return std::nullopt;

    App a;
    a.id   = id;
    a.name = local("Name");
    a.exec = stripFieldCodes(get("Exec"));
    if (a.name.empty() || a.exec.empty())
        return std::nullopt;
    a.extra    = local("GenericName") + " " + semis(local("Keywords")) + " " + get("Name") + " " + semis(get("Keywords")) + " " + id;
    a.icon     = get("Icon");
    a.tryExec  = get("TryExec");
    a.terminal = get("Terminal") == "true";
    return a;
}

namespace {

bool inPath(const std::string& prog) {
    if (prog.empty())
        return true;
    std::error_code ec;
    if (prog[0] == '/')
        return fs::exists(prog, ec);
    const char* path = getenv("PATH");
    std::string p    = path ? path : "/usr/bin:/bin";
    size_t      a    = 0;
    while (a <= p.size()) {
        size_t b = p.find(':', a);
        if (b == std::string::npos)
            b = p.size();
        if (b > a && fs::exists(fs::path(p.substr(a, b - a)) / prog, ec))
            return true;
        a = b + 1;
    }
    return false;
}

} // namespace

std::vector<App> loadApps(const std::vector<std::string>& dirs, const std::vector<std::string>& langs, std::string_view desktop) {
    std::vector<App>      out;
    std::set<std::string> seen;
    for (auto& d : dirs) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->path().extension() != ".desktop")
                continue;
            auto id = fs::relative(it->path(), d, ec).replace_extension().string();
            std::replace(id.begin(), id.end(), '/', '-');
            if (!seen.insert(id).second)
                continue; // o mesmo id numa pasta antes (inclusive um Hidden=true do usuário) ganha
            std::ifstream     f(it->path());
            std::stringstream ss;
            ss << f.rdbuf();
            if (auto a = parseDesktop(id, ss.str(), langs, desktop); a && inPath(a->tryExec))
                out.push_back(std::move(*a));
        }
    }
    std::sort(out.begin(), out.end(), [](const App& a, const App& b) { return fold(a.name) < fold(b.name); });
    return out;
}

// ---------- resultados ----------

const char* sectionName(Kind k) {
    switch (k) {
        case Kind::Calc: return "Calculadora";
        case Kind::App: return "Aplicativos";
        case Kind::Window: return "Janelas abertas";
        case Kind::File: return "Arquivos";
    }
    return "";
}

namespace {

// todas as palavras precisam casar no texto principal ou, valendo metade, no secundário
int scoreAll(const std::vector<std::string>& toks, const std::string& main, const std::string& extra) {
    int total = 0;
    for (auto& t : toks) {
        const int a = fuzzy(t, main);
        const int b = extra.empty() ? -1 : fuzzy(t, extra);
        const int s = std::max(a, b >= 0 ? b / 2 : -1);
        if (s < 0)
            return -1;
        total += s;
    }
    return total;
}

const char* fileIcon(const std::string& name, bool dir) {
    if (dir)
        return "folder";
    std::string ext;
    if (auto p = name.rfind('.'); p != std::string::npos && p > 0)
        ext = fold(name.substr(p + 1));
    static const std::map<std::string, const char*> icons = {
        {"pdf", "application-pdf"},    {"png", "image-x-generic"},  {"jpg", "image-x-generic"},  {"jpeg", "image-x-generic"},
        {"gif", "image-x-generic"},    {"webp", "image-x-generic"}, {"svg", "image-x-generic"},  {"mp3", "audio-x-generic"},
        {"flac", "audio-x-generic"},   {"ogg", "audio-x-generic"},  {"wav", "audio-x-generic"},  {"mp4", "video-x-generic"},
        {"mkv", "video-x-generic"},    {"webm", "video-x-generic"}, {"zip", "package-x-generic"}, {"gz", "package-x-generic"},
        {"xz", "package-x-generic"},   {"tar", "package-x-generic"}, {"html", "text-html"},       {"sh", "text-x-script"},
        {"py", "text-x-script"},
    };
    auto it = icons.find(ext);
    return it == icons.end() ? "text-x-generic" : it->second;
}

} // namespace

Result fileResult(const std::string& rel, const std::string& home) {
    std::string p   = rel;
    const bool  dir = p.ends_with('/');
    if (dir)
        p.pop_back();
    const auto  slash = p.rfind('/');
    std::string name  = slash == std::string::npos ? p : p.substr(slash + 1);
    std::string where = slash == std::string::npos ? "~" : "~/" + p.substr(0, slash);
    Result      r;
    r.kind     = Kind::File;
    r.title    = name;
    r.subtitle = where;
    r.key      = home + "/" + p;
    r.icon     = fileIcon(name, dir);
    return r;
}

std::vector<Result> search(std::string_view query, const std::vector<App>& apps, const std::vector<Window>& windows, const std::vector<std::string>& files,
                           const std::string& home, const std::unordered_map<std::string, int>& usage, const Limits& lim) {
    std::vector<Result> out;
    const auto          toks = words(fold(query));
    if (toks.empty())
        return out;

    if (const auto v = calc(query)) {
        const auto txt = formatNumber(*v, usesComma(query));
        out.push_back({Kind::Calc, "= " + txt, "Enter copia o resultado", txt, "=", 0});
    }

    std::vector<Result> found;
    for (auto& a : apps) {
        int s = scoreAll(toks, fold(a.name), fold(a.extra));
        if (s < 0)
            continue;
        if (auto it = usage.find(a.id); it != usage.end())
            s += std::min(it->second, 20) * 6;
        found.push_back({Kind::App, a.name, "", a.id, a.id, s});
    }
    std::stable_sort(found.begin(), found.end(), [](const Result& a, const Result& b) { return a.score > b.score; });
    found.resize(std::min(found.size(), lim.apps));
    out.insert(out.end(), found.begin(), found.end());

    found.clear();
    for (auto& w : windows) {
        const int s = scoreAll(toks, fold(w.title), fold(w.appName + " " + w.cls));
        if (s < 0)
            continue;
        found.push_back({Kind::Window, w.title.empty() ? w.appName : w.title, w.appName, w.addr, w.cls, s});
    }
    std::stable_sort(found.begin(), found.end(), [](const Result& a, const Result& b) { return a.score > b.score; });
    found.resize(std::min(found.size(), lim.windows));
    out.insert(out.end(), found.begin(), found.end());

    for (size_t i = 0; i < files.size() && i < lim.files; i++)
        out.push_back(fileResult(files[i], home));
    return out;
}

std::vector<std::string> rankFiles(std::string_view query, const std::vector<std::string>& list, size_t limit) {
    const auto toks = words(fold(query));
    if (toks.empty())
        return {};
    std::vector<std::pair<int, size_t>> hits;
    for (size_t i = 0; i < list.size(); i++) {
        const auto  path = fold(list[i]);
        std::string_view p(path);
        if (p.ends_with('/'))
            p.remove_suffix(1);
        const auto       slash = p.rfind('/');
        const std::string base(slash == std::string_view::npos ? p : p.substr(slash + 1));
        int               total = 0;
        for (auto& t : toks) {
            const int b = fuzzy(t, base);
            const int s = b >= 0 ? b * 2 : fuzzy(t, p);
            if (s < 0) {
                total = -1;
                break;
            }
            total += s;
        }
        if (total < 0)
            continue;
        total -= 5 * (int)std::count(p.begin(), p.end(), '/'); // mais perto de ~ primeiro
        hits.push_back({total, i});
    }
    const size_t n = std::min(limit, hits.size());
    std::partial_sort(hits.begin(), hits.begin() + n, hits.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    std::vector<std::string> out;
    for (size_t i = 0; i < n; i++)
        out.push_back(list[hits[i].second]);
    return out;
}

// ---------- lista ----------

std::vector<Row> layoutRows(const std::vector<Result>& results, double rowH, double headH) {
    std::vector<Row> rows;
    double           y = 0;
    for (size_t i = 0; i < results.size(); i++) {
        if (i == 0 || results[i].kind != results[i - 1].kind) {
            rows.push_back({-1, sectionName(results[i].kind), y, headH});
            y += headH;
        }
        rows.push_back({(int)i, "", y, rowH});
        y += rowH;
    }
    return rows;
}

double rowsHeight(const std::vector<Row>& rows) {
    return rows.empty() ? 0 : rows.back().y + rows.back().h;
}

double scrollFor(const std::vector<Row>& rows, int sel, double viewH, double cur) {
    const double total = rowsHeight(rows);
    for (size_t r = 0; r < rows.size(); r++) {
        if (rows[r].result != sel)
            continue;
        double top = rows[r].y;
        if (r > 0 && rows[r - 1].result < 0)
            top = rows[r - 1].y; // primeiro da seção: mostra o título junto
        const double bottom = rows[r].y + rows[r].h;
        if (top < cur)
            cur = top;
        if (bottom > cur + viewH)
            cur = bottom - viewH;
        break;
    }
    return std::clamp(cur, 0.0, std::max(0.0, total - viewH));
}

int hitRow(const std::vector<Row>& rows, double y, double scroll, double viewH) {
    if (y < 0 || y >= viewH)
        return -1;
    const double yy = y + scroll;
    for (auto& r : rows)
        if (r.result >= 0 && yy >= r.y && yy < r.y + r.h)
            return r.result;
    return -1;
}

// ---------- uso ----------

std::unordered_map<std::string, int> parseUsage(const std::string& content) {
    std::unordered_map<std::string, int> out;
    std::istringstream                   in(content);
    std::string                          line;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab + 1 >= line.size())
            continue;
        const int n = std::atoi(line.substr(0, tab).c_str());
        if (n > 0)
            out[line.substr(tab + 1)] = n;
    }
    return out;
}

std::string formatUsage(const std::unordered_map<std::string, int>& usage) {
    std::vector<std::pair<std::string, int>> v(usage.begin(), usage.end());
    std::sort(v.begin(), v.end());
    std::string out;
    for (auto& [k, n] : v)
        out += std::to_string(n) + "\t" + k + "\n";
    return out;
}

} // namespace hf
