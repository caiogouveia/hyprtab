// hyprfind - busca rápida inspirada no Spotlight do macOS para o Hyprland, tudo dentro do .so (C++).
//
// Um atalho abre uma caixa de busca no meio da tela, desenhada pelo próprio compositor. Enquanto ela
// está aberta, o teclado é dela. Resultados por seção: Calculadora, Aplicativos (.desktop), Janelas
// abertas e Arquivos (varredura do ~ numa thread, ordenada pelo fzf se houver).
//
//   bind = SUPER, SPACE, hyprfind:toggle          # hyprlang, até 0.56
//   hl.bind("SUPER + SPACE", function() hl.plugin.hyprfind.toggle() end)   -- Lua, 0.57+
//
// Config (hyprlang):
//   plugin {
//       hyprfind {
//           width = 680      # largura da caixa, px
//           files = 1        # 0: sem a seção de arquivos
//           fzf = auto       # auto, off ou o caminho do executável
//       }
//   }

#include "../../src/clients.hpp"
#include "../../src/icons.hpp"
#include "../../src/render.hpp"
#include "files.hpp"
#include "model.hpp"

#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include <pango/pangocairo.h>

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <signal.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

static constexpr uint32_t BTN_LEFT_ = 0x110;

namespace {

// ---------- medidas (px lógicos) ----------

constexpr double SEARCH_H = 58, ROW_H = 44, HEAD_H = 26, PAD = 6, RADIUS = 16;
constexpr double LIST_MAX_H = 8 * ROW_H + 2 * HEAD_H; // além disso, rola
constexpr double TOP_FRAC   = 0.22;                    // topo da caixa, fração da altura do monitor
constexpr double ICON = 28, TEXT_X = 58, SIDE = 18;
constexpr double QUERY_PX = 24, TITLE_PX = 15, SUB_PX = 13, HEAD_PX = 12;
constexpr double DAMAGE_PAD = 70; // sombra do painel

const CHyprColor ACCENT{0.04, 0.40, 0.88, 0.88};
const CHyprColor SEPARATOR{1, 1, 1, 0.10};

constexpr auto BLINK_EVERY = std::chrono::milliseconds(530);
constexpr auto DEBOUNCE    = std::chrono::milliseconds(120); // espera digitar antes de buscar arquivos
constexpr auto POLL_EVERY  = std::chrono::milliseconds(30);  // resultado da thread de arquivos
constexpr auto APPS_MAX_AGE = std::chrono::seconds(30);

// ---------- config ----------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations" // 0.57 marca a API hyprlang como deprecated

void addConfig() {
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprfind:width", Hyprlang::INT{680});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprfind:files", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprfind:fzf", Hyprlang::STRING{"auto"});
}

Hyprlang::INT cfgInt(const char* name, Hyprlang::INT def) {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, name);
    return v ? **(Hyprlang::INT* const*)v->getDataStaticPtr() : def;
}
std::string cfgStr(const char* name, const std::string& def) {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, name);
    if (!v)
        return def;
    auto s = *(Hyprlang::STRING const*)v->getDataStaticPtr();
    return s ? s : def;
}

#pragma GCC diagnostic pop

// ---------- estado ----------

std::string home() {
    const char* h = getenv("HOME");
    return h ? h : "/";
}

bool                     g_open = false;
PHLMONITORREF            g_mon;
std::string              g_query;
std::vector<hf::Result>  g_results;
std::vector<hf::Row>     g_rows;
int                      g_sel    = 0;
double                   g_scroll = 0;
double                   g_width  = 680;
bool                     g_caret  = true;
CBox                     g_lastDamage;

std::vector<hf::App>                  g_apps;
std::chrono::steady_clock::time_point g_appsAt{};
std::vector<hf::Window>               g_windows;
std::unordered_map<std::string, int>  g_usage;

// arquivos
hf::FileSearch           g_search;
bool                     g_searchStarted = false, g_filesOn = true;
std::vector<std::string> g_files; // da última busca que voltou (relativos a ~)
uint64_t                 g_gen = 0;
bool                     g_waiting = false;

// mouse
Vector2D g_mouse, g_anchor;
bool     g_moved = false, g_swallowRelease = false;

// teclado
std::set<uint32_t>    g_eaten; // teclas cujo "pressionar" foi nosso: o "soltar" também é
uint32_t              g_repeatCode = 0;
std::function<void()> g_repeatAction;
int                   g_repeatRate = 25, g_repeatDelay = 400;
xkb_context*          g_xkb     = nullptr;
xkb_compose_table*    g_compose = nullptr;
xkb_compose_state*    g_composeState = nullptr;

SP<CEventLoopTimer> g_blink, g_debounce, g_filePoll, g_repeat;
SP<SHyprCtlCommand> g_ctl;

// ---------- geometria ----------

struct Geo {
    double x = 0, y = 0, w = 0, h = 0; // painel, relativo ao monitor (px lógicos)
    double listY = 0, listH = 0;       // área visível da lista
};

Geo geometry(const PHLMONITOR& mon) {
    Geo g;
    g.w = std::min(g_width, mon->m_size.x - 32);
    g.x = std::round((mon->m_size.x - g.w) / 2);
    g.y = std::round(mon->m_size.y * TOP_FRAC);
    g.h = SEARCH_H;
    if (!g_rows.empty()) {
        // cabe no monitor: em tela baixa a lista rola antes
        const double room = std::max(ROW_H + HEAD_H, mon->m_size.y - g.y - SEARCH_H - 1 - 2 * PAD - 24);
        g.listH           = std::min({hf::rowsHeight(g_rows), LIST_MAX_H, room});
        g.listY = g.y + SEARCH_H + 1 + PAD;
        g.h += 1 + 2 * PAD + g.listH;
    }
    return g;
}

void damage() {
    auto mon = g_mon.lock();
    if (g_lastDamage.w > 0)
        g_pHyprRenderer->damageBox(g_lastDamage);
    g_lastDamage = {};
    if (!g_open || !mon)
        return;
    const auto g = geometry(mon);
    g_lastDamage = CBox{mon->m_position.x + g.x - DAMAGE_PAD, mon->m_position.y + g.y - DAMAGE_PAD, g.w + 2 * DAMAGE_PAD, g.h + 2 * DAMAGE_PAD};
    g_pHyprRenderer->damageBox(g_lastDamage);
}

// ---------- texto ----------

struct Text {
    render::Tex tex;
    int         w = 0, h = 0; // px físicos
};

// texto em px físicos: `size` é a altura da fonte; corta com reticências em `maxW` (no começo, se
// `tail`: na caixa de busca o fim do que se digita fica à vista)
Text text(const std::string& s, int size, int maxW, const CHyprColor& c, bool bold = false, bool tail = false) {
    static auto* cache = new std::unordered_map<std::string, Text>(); // ver render.hpp: nunca destruído
    if (s.empty() || maxW <= 0)
        return {};
    const auto key = std::format("{}:{}:{}:{}:{}:{}:{}:{}:{}", size, maxW, bold, tail, c.r, c.g, c.b, c.a, s);
    if (auto it = cache->find(key); it != cache->end())
        return it->second;
    if (cache->size() > 400) // só limpa dentro do desenho (contexto GL ativo)
        cache->clear();

    auto  font   = std::format("Sans {}{}px", bold ? "Bold " : "", size);
    auto* fd     = pango_font_description_from_string(font.c_str());
    auto  layout = [&](cairo_t* cr) {
        auto* l = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(l, fd);
        pango_layout_set_width(l, maxW * PANGO_SCALE);
        pango_layout_set_ellipsize(l, tail ? PANGO_ELLIPSIZE_START : PANGO_ELLIPSIZE_END);
        pango_layout_set_single_paragraph_mode(l, true);
        pango_layout_set_text(l, s.c_str(), -1);
        return l;
    };
    cairo_surface_t* probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t*         pcr   = cairo_create(probe);
    auto*            pl    = layout(pcr);
    PangoRectangle   ink, logical;
    pango_layout_get_pixel_extents(pl, &ink, &logical);
    g_object_unref(pl);
    cairo_destroy(pcr);
    cairo_surface_destroy(probe);

    Text t;
    t.w = std::clamp(logical.x + logical.width, 1, maxW);
    t.h = std::max(1, logical.height);
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, t.w, t.h);
    cairo_t*         cr   = cairo_create(surf);
    auto*            l    = layout(cr);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    pango_font_description_free(fd);
    t.tex = render::makeTexture(surf);
    cairo_surface_destroy(surf);
    return (*cache)[key] = t;
}

// lupa da caixa de busca
render::Tex lens(int px) {
    static auto* cache = new std::unordered_map<int, render::Tex>();
    if (auto it = cache->find(px); it != cache->end())
        return it->second;
    cairo_surface_t* s  = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t*         cr = cairo_create(s);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.62);
    cairo_set_line_width(cr, px * 0.1);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, px * 0.42, px * 0.42, px * 0.29, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, px * 0.64, px * 0.64);
    cairo_line_to(cr, px * 0.9, px * 0.9);
    cairo_stroke(cr);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    auto t = render::makeTexture(s);
    cairo_surface_destroy(s);
    return (*cache)[px] = t;
}

// ---------- desenho ----------

void draw(const PHLMONITOR& mon) {
    if (!g_open || hyprtabLocked() || g_mon.lock() != mon)
        return;
    const double sc = mon->m_scale;
    const auto   g  = geometry(mon);
    auto         px = [&](double v) { return v * sc; };
    // caixa em px físicos inteiros (texto nítido)
    auto at = [&](double x, double y, double w, double h) { return CBox{std::round(px(x)), std::round(px(y)), std::round(px(w)), std::round(px(h))}; };
    auto put = [&](const Text& t, double x, double y) { // x, y lógicos; tamanho já físico
        if (t.tex)
            render::drawTexture(t.tex, CBox{std::round(px(x)), std::round(px(y)), (double)t.w, (double)t.h});
    };

    render::glassPanel(CBox{px(g.x), px(g.y), px(g.w), px(g.h)}, (int)px(RADIUS), sc);

    // busca: lupa, texto (ou a dica) e o cursor
    const double lensS = 22;
    render::drawTexture(lens((int)std::ceil(px(lensS))), at(g.x + SIDE, g.y + (SEARCH_H - lensS) / 2, lensS, lensS));
    const double qx   = g.x + SIDE + lensS + 12;
    const int    qMax = (int)px(g.x + g.w - SIDE - qx);
    const auto   q    = g_query.empty() ? text("Pesquisar apps, janelas, arquivos e contas", (int)px(QUERY_PX), qMax, CHyprColor{1, 1, 1, 0.38})
                                        : text(g_query, (int)px(QUERY_PX), qMax, CHyprColor{1, 1, 1, 0.96}, false, true);
    const double qh = q.h / sc;
    put(q, qx, g.y + (SEARCH_H - qh) / 2);
    if (g_caret) {
        const double cx = g_query.empty() ? qx - 1 : qx + q.w / sc + 1, ch = QUERY_PX * 1.15;
        render::fillRect(at(cx, g.y + (SEARCH_H - ch) / 2, 2, ch), CHyprColor{0.55, 0.75, 1, 0.95}, 0);
    }
    if (g_rows.empty())
        return;

    render::fillRect(at(g.x, g.y + SEARCH_H, g.w, 1), SEPARATOR, 0);

    for (auto& r : g_rows) {
        const double top = r.y - g_scroll;
        if (top < -0.5 || top + r.h > g.listH + 0.5)
            continue; // só linhas inteiras
        const double y = g.listY + top;
        if (r.result < 0) {
            const auto h = text(r.header, (int)px(HEAD_PX), (int)px(g.w - 2 * SIDE), CHyprColor{1, 1, 1, 0.5}, true);
            put(h, g.x + SIDE, y + (r.h - h.h / sc) / 2 + 2);
            continue;
        }
        const auto& res = g_results[(size_t)r.result];
        const bool  sel = r.result == g_sel;
        if (sel)
            render::fillRect(at(g.x + 8, y + 2, g.w - 16, r.h - 4), ACCENT, (int)px(9));
        render::drawTexture(render::iconTexture(res.icon, std::max(16, (int)std::ceil(px(ICON) / 8) * 8)), at(g.x + SIDE, y + (r.h - ICON) / 2, ICON, ICON));

        const double right  = g.x + g.w - SIDE;
        const double avail  = right - (g.x + TEXT_X);
        const auto   sub    = text(res.subtitle, (int)px(SUB_PX), (int)px(avail * 0.45), CHyprColor{1, 1, 1, sel ? 0.8F : 0.5F});
        const double titleW = avail - (sub.w ? sub.w / sc + 16 : 0);
        const auto   title  = text(res.title, (int)px(res.kind == hf::Kind::Calc ? TITLE_PX + 3 : TITLE_PX), (int)px(titleW), CHyprColor{1, 1, 1, 0.97});
        put(title, g.x + TEXT_X, y + (r.h - title.h / sc) / 2);
        put(sub, right - sub.w / sc, y + (r.h - sub.h / sc) / 2);
    }
}

// ---------- resultados ----------

// altura visível da lista no monitor da caixa
double viewH() {
    auto mon = g_mon.lock();
    return mon ? geometry(mon).listH : LIST_MAX_H;
}

void refresh(bool keepSelection) {
    const std::string selKey = keepSelection && g_sel >= 0 && (size_t)g_sel < g_results.size() ? g_results[(size_t)g_sel].key : "";
    g_results = hf::search(g_query, g_apps, g_windows, g_filesOn ? g_files : std::vector<std::string>{}, home(), g_usage);
    g_rows    = hf::layoutRows(g_results, ROW_H, HEAD_H);
    g_sel     = 0;
    if (!selKey.empty())
        for (size_t i = 0; i < g_results.size(); i++)
            if (g_results[i].key == selKey)
                g_sel = (int)i;
    g_scroll = g_results.empty() ? 0 : hf::scrollFor(g_rows, g_sel, viewH(), keepSelection ? g_scroll : 0);
    damage();
}

void select(int i) {
    if (g_results.empty())
        return;
    i = std::clamp(i, 0, (int)g_results.size() - 1);
    if (i == g_sel)
        return;
    g_sel    = i;
    g_scroll = hf::scrollFor(g_rows, g_sel, viewH(), g_scroll);
    damage();
}

void queryChanged() {
    g_caret = true;
    g_gen++;
    // arquivos antigos que ainda casam ficam até a busca nova voltar
    g_files = hf::rankFiles(g_query, g_files, g_files.size());
    refresh(false);
    const auto toks = hf::words(g_query);
    std::string joined;
    for (auto& t : toks)
        joined += t;
    if (g_filesOn && joined.size() >= 2 && !hf::calc(g_query))
        g_debounce->updateTimeout(DEBOUNCE);
    else {
        g_debounce->updateTimeout(std::nullopt);
        g_waiting = false;
    }
}

void onDebounce(SP<CEventLoopTimer>, void*) {
    if (!g_open)
        return;
    g_search.request(g_gen, g_query);
    g_waiting = true;
    g_filePoll->updateTimeout(POLL_EVERY);
}

void onPoll(SP<CEventLoopTimer> self, void*) {
    uint64_t                 gen = 0;
    std::vector<std::string> out;
    if (g_search.take(gen, out) && gen == g_gen) {
        g_files   = std::move(out);
        g_waiting = false;
        if (g_open)
            refresh(true);
    }
    if (g_waiting && g_open)
        self->updateTimeout(POLL_EVERY);
}

// ---------- apps, janelas, uso ----------

std::vector<std::string> appDirs() {
    std::vector<std::string> dirs;
    const char*              dh = getenv("XDG_DATA_HOME");
    dirs.push_back((dh && *dh ? std::string(dh) : home() + "/.local/share") + "/applications");
    const char* dd   = getenv("XDG_DATA_DIRS");
    std::string list = dd && *dd ? dd : "/usr/local/share:/usr/share";
    list += ":" + home() + "/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share";
    size_t a = 0;
    while (a <= list.size()) {
        size_t b = list.find(':', a);
        if (b == std::string::npos)
            b = list.size();
        if (b > a)
            dirs.push_back(list.substr(a, b - a) + "/applications");
        a = b + 1;
    }
    dirs.push_back("/var/lib/snapd/desktop/applications");
    std::vector<std::string> out;
    for (auto& d : dirs) {
        while (d.find("//") != std::string::npos)
            d.replace(d.find("//"), 2, "/");
        if (std::find(out.begin(), out.end(), d) == out.end())
            out.push_back(d);
    }
    return out;
}

void loadAppsIfStale() {
    const auto now = std::chrono::steady_clock::now();
    if (!g_apps.empty() && now - g_appsAt < APPS_MAX_AGE)
        return;
    const char* lang = getenv("LC_ALL");
    if (!lang || !*lang)
        lang = getenv("LC_MESSAGES");
    if (!lang || !*lang)
        lang = getenv("LANG");
    const char* desk = getenv("XDG_CURRENT_DESKTOP");
    g_apps   = hf::loadApps(appDirs(), hf::localeLangs(lang ? lang : ""), desk ? desk : "Hyprland");
    g_appsAt = now;
}

void loadWindows() {
    g_windows.clear();
    for (auto& w : hyprtabWindows()) {
        if (!w || !hyprtabMapped(w) || classIgnored(w->m_class, DEFAULT_IGNORE))
            continue;
        const auto* de = icons::desktopEntry(w->m_class);
        g_windows.push_back({hyprtabAddr(w), w->m_class, w->m_title, de && !de->name.empty() ? de->name : w->m_class});
    }
}

std::string usagePath() {
    const char* sh = getenv("XDG_STATE_HOME");
    return (sh && *sh ? std::string(sh) : home() + "/.local/state") + "/hyprfind/usage";
}

void loadUsage() {
    std::ifstream     f(usagePath());
    std::stringstream ss;
    ss << f.rdbuf();
    g_usage = hf::parseUsage(ss.str());
}

void bumpUsage(const std::string& id) {
    g_usage[id]++;
    std::error_code ec;
    fs::create_directories(fs::path(usagePath()).parent_path(), ec);
    std::ofstream(usagePath()) << hf::formatUsage(g_usage);
}

// ---------- abrir, fechar, ações ----------

// roda fora do compositor: fork duplo para o processo não virar zumbi nem filho do Hyprland
void spawn(const std::string& cmd) {
    const pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), (char*)nullptr);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, nullptr, 0);
}

void stopRepeat() {
    g_repeatCode = 0;
    g_repeatAction = nullptr;
    if (g_repeat)
        g_repeat->updateTimeout(std::nullopt);
}

void closeSearch() {
    if (!g_open)
        return;
    g_open = false;
    damage();
    stopRepeat();
    g_blink->updateTimeout(std::nullopt);
    g_debounce->updateTimeout(std::nullopt);
    g_waiting = false;
    if (g_composeState)
        xkb_compose_state_reset(g_composeState);
    // a próxima abertura começa vazia
    g_query.clear();
    g_results.clear();
    g_rows.clear();
    g_files.clear();
    g_sel = 0;
    g_scroll = 0;
}

void openSearch(const std::string& initial = "") {
    if (g_open || hyprtabLocked())
        return;
    auto mon = render::currentMonitor();
    if (!mon)
        return;
    g_width   = (double)std::clamp<Hyprlang::INT>(cfgInt("plugin:hyprfind:width", 680), 320, 2000);
    g_filesOn = cfgInt("plugin:hyprfind:files", 1) != 0;
    if (g_filesOn && !g_searchStarted) {
        const auto opt = cfgStr("plugin:hyprfind:fzf", "auto");
        g_search.start(home(), opt == "off" ? "" : opt == "auto" || opt.empty() ? hf::findFzf(home()) : opt);
        g_searchStarted = true;
    }
    if (g_filesOn)
        g_search.warm();
    loadAppsIfStale();
    loadWindows();

    g_open  = true;
    g_mon   = mon;
    g_query = initial;
    g_caret = true;
    g_moved = false;
    g_anchor = g_mouse;
    g_blink->updateTimeout(BLINK_EVERY);
    queryChanged();
}

void activate(int i, bool alt) {
    if (i < 0 || (size_t)i >= g_results.size())
        return;
    const auto r = g_results[(size_t)i];
    closeSearch();
    switch (r.kind) {
        case hf::Kind::Calc: spawn("wl-copy -- " + hf::shellQuote(r.key)); break;
        case hf::Kind::Window: hyprtabFocus(r.key); break;
        case hf::Kind::File:
            // Ctrl+Enter: abre a pasta onde ele está
            spawn("xdg-open " + hf::shellQuote(alt ? fs::path(r.key).parent_path().string() : r.key) + " >/dev/null 2>&1");
            break;
        case hf::Kind::App: {
            const auto it = std::find_if(g_apps.begin(), g_apps.end(), [&](const hf::App& a) { return a.id == r.key; });
            if (it == g_apps.end())
                return;
            spawn(it->terminal ? "xdg-terminal-exec " + it->exec : it->exec);
            bumpUsage(it->id);
            break;
        }
    }
}

// ---------- teclado ----------

// Tecla de texto ou de edição. Devolve a ação para repetir enquanto a tecla estiver presa (ou nada).
std::function<void()> handleKey(xkb_keysym_t sym, const std::string& utf8, bool ctrl, bool shift) {
    auto edit = [](std::function<void()> f) {
        return [f] {
            f();
            queryChanged();
        };
    };
    switch (sym) {
        case XKB_KEY_Escape:
            if (g_query.empty())
                closeSearch();
            else {
                g_query.clear();
                queryChanged();
            }
            return nullptr;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter: activate(g_sel, ctrl); return nullptr;
        case XKB_KEY_Up:
        case XKB_KEY_KP_Up:
        case XKB_KEY_ISO_Left_Tab: return [] { select(g_sel - 1); };
        case XKB_KEY_Down:
        case XKB_KEY_KP_Down: return [] { select(g_sel + 1); };
        case XKB_KEY_Tab: return shift ? std::function<void()>([] { select(g_sel - 1); }) : [] { select(g_sel + 1); };
        case XKB_KEY_Page_Up: return [] { select(g_sel - 5); };
        case XKB_KEY_Page_Down: return [] { select(g_sel + 5); };
        case XKB_KEY_BackSpace: return ctrl ? edit([] { hf::popWord(g_query); }) : edit([] { hf::popChar(g_query); });
        default: break;
    }
    if (ctrl) {
        switch (sym) {
            case XKB_KEY_u: return edit([] { g_query.clear(); });
            case XKB_KEY_w: return edit([] { hf::popWord(g_query); });
            case XKB_KEY_n:
            case XKB_KEY_j: return [] { select(g_sel + 1); };
            case XKB_KEY_p:
            case XKB_KEY_k: return [] { select(g_sel - 1); };
            default: return nullptr;
        }
    }
    if (!utf8.empty() && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f)
        return edit([utf8] { g_query += utf8; });
    return nullptr;
}

// repetição configurada no teclado (input:repeat_rate/repeat_delay); os membros podem mudar entre versões
template <class K>
void repeatSettings(const K& kb, int& rate, int& delay) {
    rate  = 25;
    delay = 400;
    if constexpr (requires { kb->m_repeatRate, kb->m_repeatDelay; }) {
        if (kb->m_repeatRate > 0)
            rate = kb->m_repeatRate;
        if (kb->m_repeatDelay > 0)
            delay = kb->m_repeatDelay;
    }
}

bool isModifier(xkb_keysym_t sym) {
    return (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) || sym == XKB_KEY_ISO_Level3_Shift || sym == XKB_KEY_ISO_Level5_Shift || sym == XKB_KEY_Mode_switch ||
        sym == XKB_KEY_Num_Lock;
}

bool onKey(const IKeyboard::SKeyEvent& ev, SP<IKeyboard> kb) {
    if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (ev.keycode == g_repeatCode)
            stopRepeat();
        return g_eaten.erase(ev.keycode) > 0;
    }
    if (!g_open)
        return false;
    if (hyprtabLocked()) {
        closeSearch();
        return false;
    }
    if (!kb)
        kb = g_pSeatManager->m_keyboard.lock();
    xkb_state* st = kb ? kb->m_xkbState : nullptr;
    if (!st)
        return false;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(st, ev.keycode + 8);
    if (isModifier(sym))
        return false; // Shift, Ctrl etc. sozinhos seguem (o estado dos modificadores fica certo)
    auto mod = [&](const char* m) { return xkb_state_mod_name_is_active(st, m, XKB_STATE_MODS_EFFECTIVE) > 0; };
    if (mod(XKB_MOD_NAME_LOGO)) {
        if (sym == XKB_KEY_space) { // o próprio atalho: fecha
            g_eaten.insert(ev.keycode);
            closeSearch();
            return true;
        }
        closeSearch(); // outro atalho com Super: fecha e deixa o atalho acontecer
        return false;
    }
    g_eaten.insert(ev.keycode);
    stopRepeat();

    // teclas mortas (´ + a = á): passa pelo compose do locale
    std::string utf8;
    bool        composing = false;
    if (g_composeState && xkb_compose_state_feed(g_composeState, sym) == XKB_COMPOSE_FEED_ACCEPTED) {
        switch (xkb_compose_state_get_status(g_composeState)) {
            case XKB_COMPOSE_COMPOSING: composing = true; break;
            case XKB_COMPOSE_CANCELLED:
                xkb_compose_state_reset(g_composeState);
                composing = true;
                break;
            case XKB_COMPOSE_COMPOSED: {
                char buf[64];
                xkb_compose_state_get_utf8(g_composeState, buf, sizeof buf);
                utf8 = buf;
                xkb_compose_state_reset(g_composeState);
                break;
            }
            case XKB_COMPOSE_NOTHING: break;
        }
    }
    if (composing)
        return true;
    if (utf8.empty()) {
        char buf[64];
        xkb_state_key_get_utf8(st, ev.keycode + 8, buf, sizeof buf);
        utf8 = buf;
    }

    auto action = handleKey(sym, utf8, mod(XKB_MOD_NAME_CTRL), mod(XKB_MOD_NAME_SHIFT));
    if (!action)
        return true;
    action();
    if (!g_open)
        return true;
    g_repeatCode   = ev.keycode;
    g_repeatAction = action;
    repeatSettings(kb, g_repeatRate, g_repeatDelay);
    g_repeat->updateTimeout(std::chrono::milliseconds(g_repeatDelay));
    return true;
}

void onRepeat(SP<CEventLoopTimer> self, void*) {
    if (!g_open || !g_repeatAction) {
        stopRepeat();
        return;
    }
    g_repeatAction();
    if (g_repeatAction)
        self->updateTimeout(std::chrono::milliseconds(std::max(10, 1000 / g_repeatRate)));
}

void onBlink(SP<CEventLoopTimer> self, void*) {
    if (!g_open)
        return;
    g_caret = !g_caret;
    damage();
    self->updateTimeout(BLINK_EVERY);
}

// ---------- mouse ----------

// resultado sob o ponto global (ou -1); `inside`: o ponto está no painel
int resultAt(const Vector2D& pos, bool* inside = nullptr) {
    auto mon = g_mon.lock();
    if (inside)
        *inside = false;
    if (!g_open || !mon)
        return -1;
    const auto   g  = geometry(mon);
    const double lx = pos.x - mon->m_position.x, ly = pos.y - mon->m_position.y;
    if (lx < g.x || ly < g.y || lx >= g.x + g.w || ly >= g.y + g.h)
        return -1;
    if (inside)
        *inside = true;
    return g_rows.empty() ? -1 : hf::hitRow(g_rows, ly - g.listY, g_scroll, g.listH);
}

void onMouseMove(const Vector2D& pos) {
    g_mouse = pos;
    if (!g_open)
        return;
    // a caixa costuma abrir debaixo do cursor: só segue o mouse depois que ele se mexe de verdade
    if (!g_moved && std::hypot(pos.x - g_anchor.x, pos.y - g_anchor.y) < 4)
        return;
    g_moved = true;
    if (const int i = resultAt(pos); i >= 0)
        select(i);
}

bool onMouseButton(const IPointer::SButtonEvent& ev) {
    if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        const bool s     = g_swallowRelease;
        g_swallowRelease = false;
        return s;
    }
    if (!g_open)
        return false;
    g_swallowRelease = true;
    bool      inside = false;
    const int i      = resultAt(g_mouse, &inside);
    if (!inside)
        closeSearch(); // clique fora: fecha
    else if (i >= 0 && ev.button == BTN_LEFT_)
        activate(i, false);
    return true;
}

#ifdef HYPRTAB_EVENT_BUS
CHyprSignalListener g_keyListener, g_moveListener, g_buttonListener;

void registerEvents() {
    auto& ev         = Event::bus()->m_events;
    g_keyListener    = ev.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) {
        if (onKey(e, nullptr))
            info.cancelled = true;
    });
    g_moveListener   = ev.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo&) { onMouseMove(pos); });
    g_buttonListener = ev.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
        if (onMouseButton(e))
            info.cancelled = true;
    });
}
void unregisterEvents() {
    g_keyListener.reset();
    g_moveListener.reset();
    g_buttonListener.reset();
}
#else
SP<HOOK_CALLBACK_FN> g_keyCb, g_moveCb, g_buttonCb;

void registerEvents() {
    g_keyCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "keyPress", [](void*, SCallbackInfo& info, std::any data) {
        try {
            auto          map = std::any_cast<std::unordered_map<std::string, std::any>>(data);
            SP<IKeyboard> kb;
            try {
                kb = std::any_cast<SP<IKeyboard>>(map["keyboard"]);
            } catch (...) {}
            if (onKey(std::any_cast<IKeyboard::SKeyEvent>(map["event"]), kb))
                info.cancelled = true;
        } catch (...) {}
    });
    g_moveCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseMove", [](void*, SCallbackInfo&, std::any data) {
        try {
            onMouseMove(std::any_cast<Vector2D>(data));
        } catch (...) {}
    });
    g_buttonCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseButton", [](void*, SCallbackInfo& info, std::any data) {
        try {
            if (onMouseButton(std::any_cast<IPointer::SButtonEvent>(data)))
                info.cancelled = true;
        } catch (...) {}
    });
}
void unregisterEvents() {
    g_keyCb.reset();
    g_moveCb.reset();
    g_buttonCb.reset();
}
#endif

// ---------- dispatchers ----------

// hyprfind:toggle [texto]: abre (já com o texto, se vier) ou fecha
void toggle(const std::string& arg) {
    if (g_open)
        closeSearch();
    else
        openSearch(arg);
}

SDispatchResult dispToggle(std::string arg) {
    toggle(arg);
    return {};
}
int luaToggle(auto*) {
    toggle("");
    return 0;
}

void registerDispatchers() {
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprfind:toggle", dispToggle);
    [](auto h) {
        using namespace HyprlandAPI;
        if constexpr (requires { addLuaFunction(h, "hyprfind", "toggle", luaToggle); })
            addLuaFunction(h, "hyprfind", "toggle", luaToggle);
    }(PHANDLE);
}

std::string status() {
    std::string s = std::format("aberto: {}\nconsulta: \"{}\"\nselecionado: {}\napps: {}\njanelas: {}\narquivos indexados: {}\nfzf: {}\n", g_open, g_query, g_sel,
                                g_apps.size(), g_windows.size(), g_search.indexed(), g_searchStarted ? (g_search.fzf().empty() ? "(interno)" : g_search.fzf()) : "(não iniciado)");
    for (size_t i = 0; i < g_results.size(); i++)
        s += std::format("{} [{}] {} | {} | {}\n", (int)i == g_sel ? ">" : " ", hf::sectionName(g_results[i].kind), g_results[i].title, g_results[i].subtitle,
                         g_results[i].key);
    return s;
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    addConfig();
    registerEvents();
    registerDispatchers();
    render::registerRender(draw);

    g_blink    = makeShared<CEventLoopTimer>(std::nullopt, onBlink, nullptr);
    g_debounce = makeShared<CEventLoopTimer>(std::nullopt, onDebounce, nullptr);
    g_filePoll     = makeShared<CEventLoopTimer>(std::nullopt, onPoll, nullptr);
    g_repeat   = makeShared<CEventLoopTimer>(std::nullopt, onRepeat, nullptr);
    for (auto& t : {g_blink, g_debounce, g_filePoll, g_repeat})
        g_pEventLoopManager->addTimer(t);

    g_xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (g_xkb) {
        const char* loc = getenv("LC_ALL");
        if (!loc || !*loc)
            loc = getenv("LC_CTYPE");
        if (!loc || !*loc)
            loc = getenv("LANG");
        g_compose = xkb_compose_table_new_from_locale(g_xkb, loc && *loc ? loc : "C", XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (g_compose)
            g_composeState = xkb_compose_state_new(g_compose, XKB_COMPOSE_STATE_NO_FLAGS);
    }

    loadUsage();
    loadAppsIfStale();

    g_ctl = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{"hyprfind", true, [](eHyprCtlOutputFormat, std::string) { return status(); }});

    return {"hyprfind", "Busca rápida de apps, janelas e arquivos", "caio", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    closeSearch();
    for (auto* t : {&g_blink, &g_debounce, &g_filePoll, &g_repeat})
        if (*t) {
            g_pEventLoopManager->removeTimer(*t);
            t->reset();
        }
    unregisterEvents();
    render::unregisterRender();
    if (g_ctl)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_ctl);
    if (g_searchStarted)
        g_search.stop();
    if (g_composeState)
        xkb_compose_state_unref(g_composeState);
    if (g_compose)
        xkb_compose_table_unref(g_compose);
    if (g_xkb)
        xkb_context_unref(g_xkb);
    g_composeState = nullptr;
    g_compose      = nullptr;
    g_xkb          = nullptr;
}
