// hyprmenu - barra de menus global inspirada no macOS para o Hyprland, tudo dentro do .so (C++).
// Usa o código comum do hyprtab (../src): render (vidro, texturas, por era), ícones e compat.
//
// Barra no topo de cada monitor: logo (menu do sistema), nome do app em foco em negrito (menu de
// janela) e, se o app exporta, os menus dele (Arquivo, Editar, ...) lidos via D-Bus (appmenu.cpp).
// À direita: volume (pelo wpctl, volume.cpp) e relógio.
//
// Config (hyprlang, até 0.56):
//   plugin {
//       hyprmenu {
//           height = 28                  # altura da barra, px
//           font_size = 13               # px
//           reserve = 1                  # reserva a faixa: janelas não ficam por baixo
//           clock = 1
//           volume = 1                   # ícone de volume (precisa do wpctl)
//           sound_command =              # "Configurações de Som…"; vazio procura pavucontrol etc.
//           lock_command = hyprlock      # "Bloquear Tela"
//           logo =                       # caminho de um PNG/SVG; vazio usa a orquídea desenhada
//       }
//   }
//
// `hyprctl hyprmenu` mostra o estado do D-Bus (Registrar, app acompanhado, último erro).

#include "../../src/icons.hpp"
#include "../../src/render.hpp"
#include "appmenu.hpp"
#include "model.hpp"
#include "volume.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

#include <xkbcommon/xkbcommon.h>

#include <pango/pangocairo.h>

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <ctime>
#include <set>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <filesystem>
#include <fstream>

static constexpr uint32_t BTN_LEFT_ = 0x110;
// evdev
static constexpr uint32_t KEY_ESC_ = 1, KEY_ENTER_ = 28, KEY_SPACE_ = 57, KEY_KPENTER_ = 96, KEY_UP_ = 103, KEY_LEFT_ = 105, KEY_RIGHT_ = 106,
                          KEY_DOWN_ = 108;

static constexpr auto POLL_EVERY  = std::chrono::milliseconds(500); // config, relógio, estado das janelas
static constexpr auto FOCUS_EVERY = std::chrono::milliseconds(100); // troca de foco (nome do app na barra)

namespace {

constexpr double TITLE_RADIUS = 5, MENU_RADIUS = 8, ROW_RADIUS = 5;
constexpr double VOLUME_STEP = 0.05, SCROLL_STEP = 15; // rolagem: 15 unidades de delta (um "clique" da roda) = 5%
const CHyprColor BAR_COLOR{0.08, 0.08, 0.10, 0.55};
const CHyprColor ACCENT{0.04, 0.40, 0.95, 0.92}; // linha selecionada do menu

// ---------- config ----------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations" // 0.57 marca a API hyprlang como deprecated

void addConfig() {
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmenu:height", Hyprlang::INT{28});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmenu:font_size", Hyprlang::INT{13});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmenu:reserve", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmenu:clock", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmenu:shadow", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmenu:lock_command", Hyprlang::STRING{"hyprlock"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmenu:logo", Hyprlang::STRING{""});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmenu:volume", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmenu:sound_command", Hyprlang::STRING{""});
}

Hyprlang::INT cfgInt(const char* name, Hyprlang::INT def) {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, name);
    return v ? **(Hyprlang::INT* const*)v->getDataStaticPtr() : def;
}
std::string cfgStr(const char* name, const std::string& def = "") {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, name);
    if (!v)
        return def;
    auto s = *(Hyprlang::STRING const*)v->getDataStaticPtr();
    return s ? s : def;
}

#pragma GCC diagnostic pop

struct Config {
    double      height = 28, font = 13;
    bool        reserve = true, clock = true, shadow = true, volume = true;
    std::string lock = "hyprlock", logo, sound;
} g_cfg;

bool loadConfig() {
    const auto old = g_cfg.height;
    g_cfg.height   = (double)std::clamp<Hyprlang::INT>(cfgInt("plugin:hyprmenu:height", 28), 18, 64);
    g_cfg.font     = (double)std::clamp<Hyprlang::INT>(cfgInt("plugin:hyprmenu:font_size", 13), 8, 32);
    g_cfg.reserve  = cfgInt("plugin:hyprmenu:reserve", 1);
    g_cfg.clock    = cfgInt("plugin:hyprmenu:clock", 1);
    g_cfg.shadow   = cfgInt("plugin:hyprmenu:shadow", 1);
    g_cfg.lock     = cfgStr("plugin:hyprmenu:lock_command", "hyprlock");
    g_cfg.logo     = cfgStr("plugin:hyprmenu:logo");
    g_cfg.volume   = cfgInt("plugin:hyprmenu:volume", 1);
    g_cfg.sound    = cfgStr("plugin:hyprmenu:sound_command");
    return old != g_cfg.height;
}

// ---------- texto ----------
// Medida em px lógicos (para o layout) e textura em px físicos (para desenhar), ambas com cache.

PangoFontDescription* font(bool bold, double px) {
    auto* fd = pango_font_description_from_string(bold ? "Sans Bold" : "Sans");
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    return fd;
}

// tamanho em px de `s` com a fonte de `px`
std::pair<int, int> textExtent(const std::string& s, bool bold, double px) {
    static cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    static cairo_t*         cr   = cairo_create(surf);
    static PangoLayout*     lay  = pango_cairo_create_layout(cr);
    auto*                   fd   = font(bold, px);
    pango_layout_set_font_description(lay, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(lay, s.c_str(), -1);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(lay, &w, &h);
    return {w, h};
}

double textWidth(const std::string& s, bool bold) {
    static std::unordered_map<std::string, double> cache;
    const auto key = std::format("{}{}:{}", bold ? 'b' : 'r', g_cfg.font, s);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    if (cache.size() > 2000)
        cache.clear();
    return cache[key] = textExtent(s, bold, g_cfg.font).first;
}

const mb::Measure MEASURE = [](const std::string& s, bool bold) { return textWidth(s, bold); };

struct TextTex {
    render::Tex tex;
    int         w = 0, h = 0;
};

// só dentro do draw (cria e libera texturas GL)
const TextTex& textTex(const std::string& s, bool bold, double px) {
    static std::unordered_map<std::string, TextTex> cache;
    const auto key = std::format("{}{}:{}", bold ? 'b' : 'r', px, s);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    if (cache.size() > 400)
        cache.clear();
    const auto [w, h] = textExtent(s, bold, px);
    auto* surf        = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, std::max(1, w), std::max(1, h));
    auto* cr          = cairo_create(surf);
    auto* lay         = pango_cairo_create_layout(cr);
    auto* fd          = font(bold, px);
    pango_layout_set_font_description(lay, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(lay, s.c_str(), -1);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    pango_cairo_show_layout(cr, lay);
    g_object_unref(lay);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    TextTex t{render::makeTexture(surf), std::max(1, w), std::max(1, h)};
    cairo_surface_destroy(surf);
    return cache[key] = t;
}

// sombra embaixo da barra (como no OS X até o Mavericks): degradê vertical de preto até transparente,
// com queda suave (quadrática). Textura de 1 px de largura, esticada na largura do monitor.
constexpr double SHADOW_H = 6, SHADOW_A = 0.35;

render::Tex barShadowTexture(int h) {
    static std::unordered_map<int, render::Tex> cache;
    if (auto it = cache.find(h); it != cache.end())
        return it->second;
    auto*      surf   = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, h);
    auto*      data   = cairo_image_surface_get_data(surf);
    const auto stride = cairo_image_surface_get_stride(surf);
    cairo_surface_flush(surf);
    for (int y = 0; y < h; y++) {
        const double t = 1.0 - (y + 0.5) / h;
        const auto   a = (uint32_t)std::lround(255 * SHADOW_A * t * t);
        *(uint32_t*)(data + y * stride) = a << 24; // preto premultiplicado
    }
    cairo_surface_mark_dirty(surf);
    auto t = render::makeTexture(surf);
    cairo_surface_destroy(surf);
    return cache[h] = t;
}

// ---------- logo padrão: orquídea (Phalaenopsis) vista de frente, em branco ----------
// Desenhada em coordenadas 0..1: sépala em cima, duas sépalas embaixo, duas pétalas largas dos lados
// e o labelo no centro. Cada parte abre um contorno transparente antes de ser pintada, o que separa
// as partes que se sobrepõem (sem isso, em 17 px vira uma mancha).

void orchidEllipse(cairo_t* cr, double cx, double cy, double rx, double ry, double a) {
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, a);
    cairo_scale(cr, rx, ry);
    cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
    cairo_restore(cr);
}

void orchidPart(cairo_t* cr, double cx, double cy, double rx, double ry, double a, double gap) {
    orchidEllipse(cr, cx, cy, rx, ry, a);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_set_line_width(cr, gap);
    cairo_stroke_preserve(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_fill(cr);
}

void drawOrchid(cairo_t* cr, int px) {
    cairo_save(cr);
    cairo_scale(cr, px, px);
    const double gap = std::max(1.0 / px, 0.055); // traço que separa as partes (pelo menos 1 px)
    orchidPart(cr, 0.50, 0.22, 0.12, 0.20, 0.0, gap);   // sépala de cima
    orchidPart(cr, 0.34, 0.74, 0.10, 0.21, 0.55, gap);  // sépalas de baixo
    orchidPart(cr, 0.66, 0.74, 0.10, 0.21, -0.55, gap);
    orchidPart(cr, 0.25, 0.46, 0.23, 0.18, -0.30, gap); // pétalas
    orchidPart(cr, 0.75, 0.46, 0.23, 0.18, 0.30, gap);
    orchidPart(cr, 0.50, 0.60, 0.085, 0.11, 0.0, gap);  // labelo
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);        // coluna: ponto vazado no meio
    orchidEllipse(cr, 0.50, 0.47, 0.035, 0.04, 0);
    cairo_fill(cr);
    cairo_restore(cr);
}

render::Tex logoTexture(int px) {
    static std::unordered_map<std::string, render::Tex> cache;
    const auto key = std::format("{}@{}", g_cfg.logo, px);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    cairo_surface_t* surf = nullptr;
    if (!g_cfg.logo.empty())
        surf = icons::load(g_cfg.logo, px);
    else {
        surf     = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        auto* cr = cairo_create(surf);
        drawOrchid(cr, px);
        cairo_destroy(cr);
        cairo_surface_flush(surf);
    }
    auto t = render::makeTexture(surf);
    cairo_surface_destroy(surf);
    return cache[key] = t;
}

// ---------- ícone de volume ----------
// Alto-falante com três ondas; as ondas acima do nível ficam apagadas. Mudo: um "×" no lugar das ondas.

render::Tex volumeTexture(int level, bool muted, int px) {
    static std::unordered_map<int, render::Tex> cache;
    const int key = (px << 3) | (level << 1) | (muted ? 1 : 0);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    auto* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    auto* cr   = cairo_create(surf);
    cairo_scale(cr, px, px);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_move_to(cr, 0.08, 0.37); // caixa + cone
    cairo_line_to(cr, 0.26, 0.37);
    cairo_line_to(cr, 0.48, 0.16);
    cairo_line_to(cr, 0.48, 0.84);
    cairo_line_to(cr, 0.26, 0.63);
    cairo_line_to(cr, 0.08, 0.63);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_set_line_width(cr, std::max(1.2 / px, 0.075));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (muted) {
        cairo_move_to(cr, 0.64, 0.38);
        cairo_line_to(cr, 0.88, 0.62);
        cairo_move_to(cr, 0.88, 0.38);
        cairo_line_to(cr, 0.64, 0.62);
        cairo_stroke(cr);
    } else
        for (int i = 1; i <= 3; i++) {
            cairo_set_source_rgba(cr, 1, 1, 1, i <= level ? 0.95 : 0.28);
            cairo_new_sub_path(cr);
            cairo_arc(cr, 0.46, 0.5, 0.06 + 0.14 * i, -M_PI / 4, M_PI / 4);
            cairo_stroke(cr);
        }
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    auto t = render::makeTexture(surf);
    cairo_surface_destroy(surf);
    return cache[key] = t;
}

// ---------- menus da barra ----------

enum class Source { System, Window, App, Keys, Volume }; // App: menu exportado pelo app; Keys: mapa de atalhos

// ações internas (menus do sistema e da janela)
enum Act : int {
    LOCK = 1,
    SUSPEND,
    REBOOT,
    POWEROFF,
    LOGOUT,
    RELOAD,
    NEW_WINDOW,
    FULLSCREEN,
    MAXIMIZE,
    FLOAT,
    PIN,
    CLOSE_WINDOW,
    QUIT_APP,
    VOL_SLIDER,
    VOL_MUTE,
    VOL_SETTINGS,
    MOVE_WS  = 100,     // + número do workspace
    VOL_SINK = 1000000, // + id da saída no PipeWire
};

struct Top {
    std::string label;
    bool        bold = false;
    Source      source;
    mb::Node    menu;
};

std::vector<Top> g_tops;
PHLWINDOWREF     g_focus;    // janela em foco quando os menus foram montados (alvo das ações)
uint32_t         g_xid  = 0; // dela, se for X11
std::string      g_appName;  // nome mostrado em negrito

mb::Node leaf(int id, const std::string& label, bool enabled = true) {
    mb::Node n;
    n.id      = id;
    n.label   = label;
    n.enabled = enabled;
    return n;
}
mb::Node check(int id, const std::string& label, bool on, bool enabled = true) {
    auto n    = leaf(id, label, enabled);
    n.toggle  = mb::Toggle::Check;
    n.checked = on;
    return n;
}
mb::Node separator() {
    mb::Node n;
    n.separator = true;
    return n;
}

std::string appName(const PHLWINDOW& w) {
    if (!w)
        return "Hyprland";
    const auto* de = icons::desktopEntry(w->m_class);
    if (de && !de->name.empty())
        return de->name;
    return w->m_class.empty() ? w->m_title : w->m_class;
}

mb::Node systemMenu() {
    mb::Node m;
    m.children = {leaf(LOCK, "Bloquear Tela"), leaf(SUSPEND, "Suspender"), separator(), leaf(REBOOT, "Reiniciar"), leaf(POWEROFF, "Desligar"), separator(),
                  leaf(RELOAD, "Recarregar Configuração"), leaf(LOGOUT, "Encerrar Sessão")};
    return m;
}

mb::Node windowMenu(const PHLWINDOW& w, const std::string& name) {
    mb::Node m;
    if (!w) {
        m.children = {leaf(RELOAD, "Recarregar Configuração")};
        return m;
    }
    const auto* de = icons::desktopEntry(w->m_class);
    mb::Node    ws;
    ws.label   = "Mover para Workspace";
    ws.submenu = true;
    const int cur = w->m_workspace ? (int)w->workspaceID() : 0;
    for (int i = 1; i <= 10; i++)
        ws.children.push_back(check(MOVE_WS + i, std::format("Workspace {}", i), i == cur));

    m.children = {leaf(NEW_WINDOW, "Nova Janela", de && !de->exec.empty()),
                  separator(),
                  check(FULLSCREEN, "Tela Cheia", w->isFullscreen()),
                  leaf(MAXIMIZE, "Maximizar"),
                  check(FLOAT, "Flutuante", w->m_isFloating),
                  check(PIN, "Em Todos os Workspaces", w->m_pinned, w->m_isFloating), // o Hyprland só fixa janela flutuante
                  ws,
                  separator(),
                  leaf(CLOSE_WINDOW, "Fechar Janela"),
                  leaf(QUIT_APP, std::format("Encerrar {}", name))};
    return m;
}

bool inPath(const std::string& name) {
    const char* path = getenv("PATH");
    std::string dirs = path ? path : "/usr/bin:/bin";
    size_t      a    = 0;
    while (a <= dirs.size()) {
        auto b = dirs.find(':', a);
        if (b == std::string::npos)
            b = dirs.size();
        if (b > a && access((dirs.substr(a, b - a) + "/" + name).c_str(), X_OK) == 0)
            return true;
        a = b + 1;
    }
    return false;
}

// comando de "Configurações de Som…": o da config ou o primeiro mixer instalado
std::string soundCommand() {
    if (!g_cfg.sound.empty())
        return g_cfg.sound;
    static const std::pair<const char*, const char*> APPS[] = {
        {"pavucontrol", "pavucontrol"}, {"pwvucontrol", "pwvucontrol"}, // fora do GNOME o gnome-control-center se recusa a abrir ("only supported under GNOME and Unity")
        {"gnome-control-center", "env XDG_CURRENT_DESKTOP=GNOME gnome-control-center sound"}, {"systemsettings", "systemsettings kcm_pulseaudio"}};
    for (auto& [bin, cmd] : APPS)
        if (inPath(bin))
            return cmd;
    return "";
}

bool volumeShown() {
    return g_cfg.volume && volume::state().ok;
}

mb::Node volumeMenu() {
    const auto& st = volume::state();
    mb::Node    m, title, sl;
    title.header = true;
    title.label  = "Volume";
    sl.id        = VOL_SLIDER;
    sl.slider    = true;
    sl.value     = std::clamp(st.vol, 0.0, 1.0);
    m.children   = {title, sl, check(VOL_MUTE, "Sem Som", st.muted)};
    // compacto como no macOS: o menu fica com a largura do slider e os nomes das saídas são cortados com "…"
    m.maxLabelW = mb::SLIDER_MIN_W - 2 * mb::MENU_PADX - 2 * mb::ROW_PADX - mb::CHECK_W;
    if (st.sinks.size() > 1) {
        mb::Node out;
        out.header = true;
        out.label  = "Saída";
        m.children.push_back(separator());
        m.children.push_back(out);
        for (auto& s : st.sinks) {
            auto n   = check(VOL_SINK + s.id, s.label, s.def);
            n.toggle = mb::Toggle::Radio;
            m.children.push_back(n);
        }
    }
    m.children.push_back(separator());
    m.children.push_back(leaf(VOL_SETTINGS, "Configurações de Som…", !soundCommand().empty()));
    return m;
}

uint32_t xidOf(const PHLWINDOW& w) {
    if (!w || !w->m_isX11)
        return 0;
    const auto xs = w->m_xwaylandSurface.lock();
    return xs ? xs->m_xID : 0;
}

// ---------- mapas de atalhos ----------
// Um JSON por app (formato em model.hpp). Lidos de ~/.config/hyprmenu/apps (do usuário, ganham) e da pasta
// apps/ ao lado do .so (os que vêm com o plugin). Relidos quando algum arquivo muda.

std::vector<mb::AppMap>  g_maps;
std::vector<std::string> g_mapErrors;
std::string              g_mapsSig; // caminhos + datas: muda quando um arquivo muda

std::vector<std::filesystem::path> mapDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x)
        dirs.emplace_back(std::filesystem::path(x) / "hyprmenu/apps");
    else if (const char* h = getenv("HOME"))
        dirs.emplace_back(std::filesystem::path(h) / ".config/hyprmenu/apps");
    Dl_info info{};
    if (dladdr((void*)&mapDirs, &info) && info.dli_fname)
        dirs.emplace_back(std::filesystem::path(info.dli_fname).parent_path() / "apps");
    return dirs;
}

std::vector<std::filesystem::path> mapFiles(std::string& sig) {
    std::vector<std::filesystem::path> files;
    std::set<std::string>              names; // o do usuário esconde o de mesmo nome que vem com o plugin
    for (auto& dir : mapDirs()) {
        std::error_code                    ec;
        std::vector<std::filesystem::path> here;
        for (auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.path().extension() == ".json" && e.is_regular_file(ec))
                here.push_back(e.path());
        std::sort(here.begin(), here.end());
        for (auto& f : here)
            if (names.insert(f.filename().string()).second) {
                files.push_back(f);
                sig += f.string() + ":" + std::to_string(std::filesystem::last_write_time(f, ec).time_since_epoch().count()) + ";";
            }
    }
    return files;
}

// devolve true se os mapas mudaram
bool loadMaps() {
    std::string sig;
    const auto  files = mapFiles(sig);
    if (sig == g_mapsSig)
        return false;
    g_mapsSig = sig;
    g_maps.clear();
    g_mapErrors.clear();
    for (auto& f : files) {
        std::ifstream in(f);
        std::string   text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        mb::AppMap    m;
        std::string   err;
        if (mb::parseAppMap(text, m, err)) {
            m.file = f.string();
            g_maps.push_back(std::move(m));
        } else
            g_mapErrors.push_back(std::format("{}: {}", f.string(), err));
    }
    return true;
}

// manda o atalho (um ou mais acordes) para a janela dos menus
void sendKeys(const std::string& keys) {
    const auto w = g_focus.lock();
    if (!w)
        return;
    const auto addr = hyprtabAddr(w);
    for (auto& c : mb::parseKeys(keys))
        hyprtabDispatch(std::format("sendshortcut {}, {}, address:{}", c.mods, c.key, addr),
                        // 0.57+ (Lua): nome do dispatcher ainda não conferido numa 0.57 real
                        std::format("hl.dsp.send_shortcut({{ mods = '{}', key = '{}', window = 'address:{}' }})", c.mods, c.key, addr));
}

std::string mapsStatus() {
    std::string s = std::format("mapas de atalhos: {}", g_maps.size());
    for (auto& d : mapDirs())
        s += "\n  pasta: " + d.string();
    for (auto& e : g_mapErrors)
        s += "\n  erro: " + e;
    return s;
}

struct Open {
    int                 top = -1;      // menu aberto (-1: nenhum)
    std::vector<size_t> path;          // submenus abertos: linha de cada nível que abriu o próximo
    std::vector<int>    hover;         // linha destacada por nível (path.size() + 1 níveis)
    PHLMONITORREF       mon;
    std::vector<int>    openedIds;     // ids do dbusmenu avisados com "opened" (para mandar "closed")
} g_open;

void closeMenus(const char* why = "");
void damageAll();

// remonta os títulos e menus a partir da janela em foco e do menu exportado por ela
void rebuild() {
    const auto w = hyprtabFocusedWindow();
    g_focus      = w;
    g_xid        = xidOf(w);
    g_appName    = appName(w);

    std::vector<Top> tops;
    tops.push_back({"", false, Source::System, systemMenu()});
    tops.push_back({g_appName, true, Source::Window, windowMenu(w, g_appName)});
    // menu exportado pelo app; sem ele, o mapa de atalhos do app (se houver)
    if (const auto* root = appmenu::menuFor(g_xid)) {
        for (auto& c : root->children)
            if (!c.separator && !c.label.empty())
                tops.push_back({c.label, false, Source::App, c});
    } else if (const auto* map = w ? mb::findAppMap(g_maps, {w->m_class, w->m_initialClass}) : nullptr)
        for (auto& c : map->menus.children)
            tops.push_back({c.label, false, Source::Keys, c});
    if (volumeShown())
        tops.push_back({"", false, Source::Volume, volumeMenu()}); // sempre o último: fica à direita

    const bool titlesChanged = tops.size() != g_tops.size() ||
        !std::equal(tops.begin(), tops.end(), g_tops.begin(), [](const Top& a, const Top& b) { return a.label == b.label && a.source == b.source; });
    g_tops = std::move(tops);

    if (g_open.top >= 0) {
        if (g_open.top >= (int)g_tops.size()) {
            closeMenus("rebuild");
            return;
        }
        // caminhos que deixaram de existir fecham
        auto& root = g_tops[g_open.top].menu;
        while (!g_open.path.empty()) {
            const auto* n = mb::at(root, g_open.path);
            if (n && n->submenu)
                break;
            g_open.path.pop_back();
        }
        g_open.hover.resize(g_open.path.size() + 1, -1);
        for (size_t k = 0; k < g_open.hover.size(); k++) {
            std::vector<size_t> p(g_open.path.begin(), g_open.path.begin() + k);
            const auto*         n = mb::at(root, p);
            if (!n || g_open.hover[k] >= (int)n->children.size())
                g_open.hover[k] = -1;
        }
        damageAll();
    } else if (titlesChanged)
        damageAll();
}

// ---------- ações ----------

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

void runAct(int id) {
    const auto w    = g_focus.lock();
    const auto addr = w ? hyprtabAddr(w) : "";
    const auto win  = std::format("'address:{}'", addr);
    switch (id) {
        case LOCK: spawn(g_cfg.lock); return;
        case SUSPEND: spawn("systemctl suspend"); return;
        case REBOOT: spawn("systemctl reboot"); return;
        case POWEROFF: spawn("systemctl poweroff"); return;
        case LOGOUT: hyprtabDispatch("exit", "hl.dsp.exit()"); return;
        case RELOAD: HyprlandAPI::invokeHyprctlCommand("reload", ""); return;
        case VOL_MUTE: volume::toggleMute(); return;
        case VOL_SETTINGS:
            if (const auto c = soundCommand(); !c.empty())
                spawn(c);
            return;
        default:
            if (id > VOL_SINK) {
                volume::setDefault(id - VOL_SINK);
                return;
            }
            break;
    }
    if (!w)
        return;
    switch (id) {
        case NEW_WINDOW:
            if (const auto* de = icons::desktopEntry(w->m_class); de && !de->exec.empty())
                spawn(mb::stripFieldCodes(de->exec));
            break;
        case FULLSCREEN:
            hyprtabFocus(addr);
            hyprtabDispatch("fullscreen 0", std::format("hl.dsp.window.fullscreen({{ mode = 'fullscreen', window = {} }})", win));
            break;
        case MAXIMIZE:
            hyprtabFocus(addr);
            hyprtabDispatch("fullscreen 1", std::format("hl.dsp.window.fullscreen({{ mode = 'maximized', window = {} }})", win));
            break;
        case FLOAT:
            hyprtabDispatch(std::format("togglefloating address:{}", addr), std::format("hl.dsp.window.float({{ action = 'toggle', window = {} }})", win));
            break;
        case PIN: hyprtabDispatch(std::format("pin address:{}", addr), std::format("hl.dsp.window.pin({{ window = {} }})", win)); break;
        case CLOSE_WINDOW: hyprtabDispatch(std::format("closewindow address:{}", addr), std::format("hl.dsp.window.close({{ window = {} }})", win)); break;
        case QUIT_APP:
            for (auto& o : hyprtabWindows())
                if (o && hyprtabMapped(o) && o->m_class == w->m_class) {
                    const auto a = hyprtabAddr(o);
                    hyprtabDispatch(std::format("closewindow address:{}", a), std::format("hl.dsp.window.close({{ window = 'address:{}' }})", a));
                }
            break;
        default:
            if (id > MOVE_WS && id <= MOVE_WS + 10) {
                const int n = id - MOVE_WS;
                hyprtabDispatch(std::format("movetoworkspace {},address:{}", n, addr),
                                std::format("hl.dsp.window.move({{ workspace = {}, window = {} }})", n, win));
            }
    }
}

// ---------- menus abertos ----------

const mb::Node* levelMenu(size_t k) {
    if (g_open.top < 0 || g_open.top >= (int)g_tops.size() || k > g_open.path.size())
        return nullptr;
    std::vector<size_t> p(g_open.path.begin(), g_open.path.begin() + k);
    return mb::at(g_tops[g_open.top].menu, p);
}

bool isApp() {
    return g_open.top >= 0 && g_open.top < (int)g_tops.size() && g_tops[g_open.top].source == Source::App;
}

// avisa o app que um menu dele vai abrir (ele pode preencher os itens agora)
void notifyOpened(int id) {
    if (!isApp() || std::find(g_open.openedIds.begin(), g_open.openedIds.end(), id) != g_open.openedIds.end())
        return;
    g_open.openedIds.push_back(id);
    appmenu::opened(id);
}

void notifyClosedAll() {
    if (isApp())
        for (auto it = g_open.openedIds.rbegin(); it != g_open.openedIds.rend(); ++it)
            appmenu::closed(*it);
    g_open.openedIds.clear();
}

void openTop(int t, const PHLMONITOR& mon) {
    if (g_open.top >= 0)
        notifyClosedAll();
    rebuild(); // estado atual das janelas (marcas de Tela Cheia, Flutuante...)
    if (t < 0 || t >= (int)g_tops.size())
        return;
    g_open = {t, {}, {-1}, mon, {}};
    if (isApp())
        notifyOpened(g_tops[t].menu.id);
    if (g_tops[t].source == Source::Volume)
        volume::refresh(true); // lista de saídas
    damageAll();
}

std::string g_closeWhy;
void closeMenus(const char* why) {
    if (g_open.top < 0)
        return;
    g_closeWhy = why;
    notifyClosedAll();
    g_open = {};
    damageAll();
}

// mouse/teclado na linha `row` do nível `k` (-1: em nenhuma linha). Linha com submenu abre o submenu;
// outra linha fecha os submenus mais fundos. Fora de qualquer linha, um submenu aberto continua aberto
// e a linha que o abriu continua marcada (como no macOS).
void setHover(size_t k, int row) {
    const auto* menu = levelMenu(k);
    if (!menu)
        return;
    if (row >= (int)menu->children.size() || (row >= 0 && !mb::selectable(menu->children[row])))
        row = -1;
    if (row >= 0) {
        const bool same = g_open.path.size() > k && g_open.path[k] == (size_t)row;
        if (!same) {
            g_open.path.resize(k);
            if (menu->children[row].submenu) {
                g_open.path.push_back(row);
                notifyOpened(menu->children[row].id);
            }
        }
    }
    g_open.hover.resize(g_open.path.size() + 1, -1);
    if (row >= 0 || g_open.path.size() <= k)
        g_open.hover[k] = row;
}

void activateRow(size_t k, int row) {
    const auto* menu = levelMenu(k);
    if (!menu || row < 0 || row >= (int)menu->children.size())
        return;
    const auto& n = menu->children[row];
    if (!mb::selectable(n) || n.submenu || n.slider) // o slider muda arrastando, não fecha o menu
        return;
    const int    id     = n.id;
    const auto   keys   = n.keys;
    const Source source = g_tops[g_open.top].source;
    closeMenus("activate");
    if (source == Source::App)
        appmenu::activate(id);
    else if (source == Source::Keys)
        sendKeys(keys);
    else
        runAct(id);
}

// ---------- geometria por monitor ----------

double barH() {
    return g_cfg.height;
}

std::string g_clock;

// títulos da barra: os menus a partir da esquerda e, por último, o volume à direita (antes do relógio)
std::vector<mb::Title> titles(const PHLMONITOR& mon) {
    std::vector<double> w;
    const bool          vol = !g_tops.empty() && g_tops.back().source == Source::Volume;
    for (size_t i = 0; i + (vol ? 1 : 0) < g_tops.size(); i++)
        w.push_back(g_tops[i].label.empty() ? 0 : textWidth(g_tops[i].label, g_tops[i].bold));
    auto out = mb::layoutTitles(w);
    if (vol)
        out.push_back(mb::volumeTitle(mon ? mon->m_size.x : 0, g_cfg.clock && !g_clock.empty() ? textWidth(g_clock, false) : 0));
    return out;
}

std::vector<mb::Dropdown> dropdowns(const PHLMONITOR& mon) {
    std::vector<mb::Dropdown> out;
    if (g_open.top < 0 || g_open.mon.lock() != mon)
        return out;
    const auto ts = titles(mon);
    if (g_open.top >= (int)ts.size())
        return out;
    const auto* m0 = levelMenu(0);
    if (!m0 || m0->children.empty())
        return out;
    out.push_back(mb::dropdown(*m0, MEASURE, ts[g_open.top], barH(), mon->m_size.x, mon->m_size.y));
    for (size_t k = 0; k < g_open.path.size(); k++) {
        const auto* m = levelMenu(k + 1);
        if (!m || m->children.empty())
            break;
        out.push_back(mb::submenu(*m, MEASURE, out.back(), g_open.path[k], mon->m_size.x, mon->m_size.y));
    }
    return out;
}

bool barHidden(const PHLMONITOR& mon) {
    return hyprtabLocked() || hyprtabHasFullscreen(mon);
}

// monitor (com barra visível) sob o ponto global
PHLMONITOR monitorAt(const Vector2D& pos) {
    for (auto& m : g_pCompositor->m_monitors)
        if (m && m->logicalBox().containsPoint(pos))
            return m;
    return nullptr;
}

// ---------- desenho ----------

void draw(const PHLMONITOR& mon) {
    if (barHidden(mon))
        return;
    const double sc = mon->m_scale, W = mon->m_size.x, H = barH();
    auto         px = [&](double v) { return v * sc; };
    auto         tx = [&](const TextTex& t, double x, double y, double h, float a = 1.F) { // texto centrado na vertical em [y, y+h)
        render::drawTexture(t.tex, CBox{std::round(px(x)), std::round(px(y) + (px(h) - t.h) / 2), (double)t.w, (double)t.h}, a);
    };
    const double fpx = px(g_cfg.font);

    render::fillRect(CBox{0, 0, px(W), px(H)}, BAR_COLOR, 0, true);
    // borda de baixo, como a do vidro do hyprtab/dock: traço claro (reflexo) e, por fora, traço escuro
    const double lw = std::max(1.0, std::round(sc));
    render::fillRect(CBox{0, std::round(px(H)) - 2 * lw, px(W), lw}, CHyprColor{1, 1, 1, 0.22}, 0);
    render::fillRect(CBox{0, std::round(px(H)) - lw, px(W), lw}, CHyprColor{0, 0, 0, 0.45}, 0);
    if (g_cfg.shadow) {
        const int sh = std::max(2, (int)std::lround(px(SHADOW_H)));
        render::drawTexture(barShadowTexture(sh), CBox{0, std::round(px(H)), px(W), (double)sh});
    }

    const auto ts   = titles(mon);
    const bool here = g_open.top >= 0 && g_open.mon.lock() == mon;
    for (size_t i = 0; i < ts.size() && i < g_tops.size(); i++) {
        if (here && (int)i == g_open.top)
            render::fillRect(CBox{px(ts[i].x), px(3), px(ts[i].w), px(H - 6)}, CHyprColor{1, 1, 1, 0.18}, (int)px(TITLE_RADIUS));
        if (i == 0) {
            const double s = std::round(H * 0.66);
            render::drawTexture(logoTexture((int)std::lround(px(s))), CBox{std::round(px(ts[i].x + (ts[i].w - s) / 2)), std::round(px((H - s) / 2)), px(s), px(s)});
        } else if (g_tops[i].source == Source::Volume) {
            const auto&  st = volume::state();
            const double s  = std::round(H * 0.64);
            render::drawTexture(volumeTexture(mb::volumeLevel(st.vol, st.muted), st.muted, (int)std::lround(px(s))),
                                CBox{std::round(px(ts[i].x + (ts[i].w - s) / 2)), std::round(px((H - s) / 2)), px(s), px(s)});
        } else
            tx(textTex(g_tops[i].label, g_tops[i].bold, fpx), ts[i].x + mb::TITLE_PADX, 0, H);
    }

    if (g_cfg.clock && !g_clock.empty()) {
        const auto& t = textTex(g_clock, false, fpx);
        tx(t, W - mb::CLOCK_PADX - t.w / sc, 0, H);
    }

    // menus suspensos
    const auto ds = dropdowns(mon);
    for (size_t k = 0; k < ds.size(); k++) {
        const auto* menu = levelMenu(k);
        if (!menu)
            break;
        const auto& d = ds[k];
        render::glassPanel(CBox{px(d.box.x), px(d.box.y), px(d.box.w), px(d.box.h)}, (int)px(MENU_RADIUS), sc, 0.5);
        const double arrowW = std::any_of(menu->children.begin(), menu->children.end(), [](const mb::Node& n) { return n.submenu; }) ? mb::ARROW_W : 0;
        for (size_t r = 0; r < d.rows.size() && r < menu->children.size(); r++) {
            const auto& row = d.rows[r];
            const auto& n   = menu->children[r];
            if (n.separator) {
                render::fillRect(CBox{px(row.x + mb::ROW_PADX), std::round(px(row.y + row.h / 2)), px(row.w - 2 * mb::ROW_PADX), std::max(1.0, std::round(sc))},
                                 CHyprColor{1, 1, 1, 0.14}, 0);
                continue;
            }
            const bool  hot = k < g_open.hover.size() && g_open.hover[k] == (int)r;
            const float a   = n.enabled ? 1.F : 0.35F;
            if (n.header) {
                tx(textTex(n.label, true, fpx), row.x + mb::ROW_PADX, row.y, row.h, 0.6F);
                continue;
            }
            if (n.slider) {
                if (hot) // pelo teclado: ← / → mudam
                    render::fillRect(CBox{px(row.x), px(row.y), px(row.w), px(row.h)}, CHyprColor{1, 1, 1, 0.08}, (int)px(ROW_RADIUS));
                const auto   t    = mb::sliderTrack(row, textWidth("100%", false));
                const double r    = mb::SLIDER_KNOB_R;
                const double fill = t.w * n.value;
                const double ty = std::round(px(t.y)), th = std::max(2.0, std::round(px(t.h)));
                render::fillRect(CBox{px(t.x), ty, px(t.w), th}, CHyprColor{1, 1, 1, 0.20}, (int)(th / 2));
                if (fill > 0.5)
                    render::fillRect(CBox{px(t.x), ty, px(fill), th}, volume::state().muted ? CHyprColor{1, 1, 1, 0.45} : ACCENT, (int)(th / 2));
                const double kx = t.x + fill - r, ky = t.y + t.h / 2 - r;
                render::fillRect(CBox{std::round(px(kx)), std::round(px(ky)), std::round(px(2 * r)), std::round(px(2 * r))}, CHyprColor{1, 1, 1, 0.95},
                                 (int)std::round(px(r)));
                const auto& pct = textTex(std::format("{}%", (int)std::lround(n.value * 100)), false, fpx);
                tx(pct, row.x + row.w - mb::ROW_PADX - pct.w / sc, row.y, row.h, 0.75F);
                continue;
            }
            if (hot)
                render::fillRect(CBox{px(row.x), px(row.y), px(row.w), px(row.h)}, ACCENT, (int)px(ROW_RADIUS));
            if (n.toggle != mb::Toggle::None && n.checked)
                tx(textTex(n.toggle == mb::Toggle::Radio ? "•" : "✓", false, fpx), row.x + mb::ROW_PADX - 3, row.y, row.h, a);
            // texto comprido: corta com "…" no espaço que sobra (o menu tem largura máxima)
            const double scW    = n.shortcut.empty() ? 0 : mb::SHORT_GAP + textWidth(n.shortcut, false);
            const double labelW = std::min(mb::LABEL_MAX_W, row.w - 2 * mb::ROW_PADX - mb::CHECK_W - arrowW - scW);
            tx(textTex(mb::ellipsize(n.label, labelW, false, MEASURE), false, fpx), row.x + mb::ROW_PADX + mb::CHECK_W, row.y, row.h, a);
            if (!n.shortcut.empty()) {
                const auto& s = textTex(n.shortcut, false, fpx);
                tx(s, row.x + row.w - mb::ROW_PADX - arrowW - s.w / sc, row.y, row.h, hot ? 0.85F : 0.5F * a);
            }
            if (n.submenu) {
                const auto& s = textTex("›", false, px(g_cfg.font * 1.2));
                tx(s, row.x + row.w - mb::ROW_PADX - s.w / sc, row.y, row.h, a);
            }
        }
    }
}

void damageAll() {
    for (auto& m : g_pCompositor->m_monitors)
        if (m)
            g_pHyprRenderer->damageMonitor(m);
}

void damageBars() {
    for (auto& m : g_pCompositor->m_monitors)
        if (m)
            g_pHyprRenderer->damageBox(CBox{m->m_position.x, m->m_position.y, m->m_size.x, barH() + SHADOW_H + 2});
}

// ---------- reserva de espaço ----------
// O Hyprland zera a parte "LS" da área reservada de cada monitor no começo de todo rearranjo de layers
// (CReservedArea::resetType) e depois soma o que as barras pedem. Um hook nesse reset soma a faixa da
// barra logo depois de zerar: as layers (waybar etc.) são arranjadas abaixo dela e as janelas também.
// É outra função que a da dock (arrangeLayersForMonitor), então os dois plugins convivem.

#ifdef HYPRTAB_RESERVE
CFunctionHook*                             g_resetHook = nullptr;
std::unordered_map<MONITORID, double>      g_reserved;
typedef void (*ResetFn)(Desktop::CReservedArea*, Desktop::eReservedDynamicType);

double reserveFor(const PHLMONITOR& mon) {
    (void)mon; // tela cheia ignora a área reservada sozinha; não precisa rearranjar ao entrar/sair
    return g_cfg.reserve ? barH() : 0;
}

void hkReset(Desktop::CReservedArea* self, Desktop::eReservedDynamicType t) {
    ((ResetFn)g_resetHook->m_original)(self, t);
    if (t != Desktop::RESERVED_DYNAMIC_TYPE_LS)
        return;
    for (auto& mon : g_pCompositor->m_monitors)
        if (mon && &mon->m_reservedArea == self) {
            const double r       = reserveFor(mon);
            g_reserved[mon->m_id] = r;
            if (r > 0)
                self->addType(Desktop::RESERVED_DYNAMIC_TYPE_LS, Vector2D{0.0, r}, Vector2D{0.0, 0.0});
            return;
        }
}

void installReserve() {
    for (auto& m : HyprlandAPI::findFunctionsByName(PHANDLE, "resetType"))
        if (m.demangled.contains("CReservedArea::resetType")) {
            g_resetHook = HyprlandAPI::createFunctionHook(PHANDLE, m.address, (void*)&hkReset);
            if (g_resetHook && !g_resetHook->hook())
                g_resetHook = nullptr;
            break;
        }
}

void syncReserve() {
    if (!g_resetHook)
        return;
    for (auto& mon : g_pCompositor->m_monitors)
        if (mon && (!g_reserved.contains(mon->m_id) || g_reserved[mon->m_id] != reserveFor(mon)))
            g_pHyprRenderer->arrangeLayersForMonitor(mon->m_id);
}

void removeReserve() {
    if (!g_resetHook)
        return;
    HyprlandAPI::removeFunctionHook(PHANDLE, g_resetHook);
    g_resetHook = nullptr;
    for (auto& mon : g_pCompositor->m_monitors)
        if (mon)
            g_pHyprRenderer->arrangeLayersForMonitor(mon->m_id);
}
#else
void installReserve() {}
void syncReserve() {}
void removeReserve() {}
#endif

// ---------- timers ----------

SP<CEventLoopTimer> g_pollTimer, g_focusTimer, g_soon;
PHLWINDOWREF        g_lastFocus;
uint32_t            g_lastXid = 0;

void updateClock() {
    const std::time_t now = std::time(nullptr);
    std::tm           lt{};
    localtime_r(&now, &lt);
    const auto c = mb::clockText(lt.tm_wday, lt.tm_mday, lt.tm_mon, lt.tm_hour, lt.tm_min);
    if (c != g_clock) {
        g_clock = c;
        damageBars();
    }
}

void onPoll(SP<CEventLoopTimer> self, void*) {
    static unsigned tick = 0;
    if (loadConfig())
        damageAll();
    // volume mudado por fora (teclas de volume, outro mixer): a cada 1 s, ou a cada 0,5 s com o menu aberto
    const bool volOpen = g_open.top >= 0 && g_open.top < (int)g_tops.size() && g_tops[g_open.top].source == Source::Volume;
    if (g_cfg.volume && (++tick % 2 == 0 || volOpen))
        volume::refresh(volOpen);
    updateClock();
    syncReserve();
    loadMaps();
    if (g_open.top < 0) // com menu aberto o conteúdo não muda debaixo do mouse
        rebuild();
    self->updateTimeout(POLL_EVERY);
}

void onFocusTimer(SP<CEventLoopTimer> self, void*) {
    const auto w = hyprtabFocusedWindow();
    if (w != g_lastFocus.lock() || xidOf(w) != g_lastXid || appName(w) != g_appName) {
        g_lastFocus = w;
        g_lastXid   = xidOf(w);
        if (g_open.top >= 0 && w != g_focus.lock())
            closeMenus("foco"); // o foco mudou (atalho, outro app abriu): o menu era da janela anterior
        rebuild();
    }
    self->updateTimeout(FOCUS_EVERY);
}

// o app mudou o menu (chamado de dentro do processamento do D-Bus): remonta fora dele
void onMenusChanged() {
    if (g_soon && !g_soon->armed())
        g_soon->updateTimeout(std::chrono::milliseconds(1));
}

void onSoon(SP<CEventLoopTimer>, void*) {
    rebuild();
    damageBars(); // ícone de volume
}

void onVolumeChanged() {
    onMenusChanged();
}

// ---------- mouse ----------

Vector2D g_mouse;
int      g_dragRow        = -1; // arrastando o slider desta linha (nível 0 do menu de volume)
bool     g_swallowRelease = false;
bool     g_pressOpened    = false;
bool     g_mouseInMenu    = false; // o último movimento foi dentro de um menu suspenso // o clique que abriu o menu: soltar no título não fecha

// slider da linha `row` do primeiro nível, no ponto x (local ao monitor)
bool slideTo(const std::vector<mb::Dropdown>& ds, int row, double lx) {
    const auto* menu = levelMenu(0);
    if (ds.empty() || !menu || row < 0 || row >= (int)menu->children.size() || row >= (int)ds[0].rows.size() || !menu->children[row].slider)
        return false;
    volume::setVolume(mb::sliderValueAt(ds[0].rows[row], textWidth("100%", false), lx));
    return true;
}

void onMouseMove(const Vector2D& pos) {
    g_mouse = pos;
    if (g_open.top < 0)
        return;
    auto mon = g_open.mon.lock();
    if (!mon) {
        closeMenus("monitor");
        return;
    }
    const double lx = pos.x - mon->m_position.x, ly = pos.y - mon->m_position.y;
    const auto   ds = dropdowns(mon);
    if (g_dragRow >= 0) {
        slideTo(ds, g_dragRow, lx);
        return;
    }
    const auto   before = std::make_pair(g_open.path, g_open.hover);

    bool inside = false;
    for (int k = (int)ds.size() - 1; k >= 0; k--) {
        const int r = mb::hitRow(ds[k], lx, ly);
        if (r == -2)
            continue;
        inside = true;
        setHover(k, r >= 0 ? r : -1);
        break;
    }
    if (!inside) {
        if (ly >= 0 && ly < barH() && mon->logicalBox().containsPoint(pos)) {
            const int t = mb::hitTitle(titles(mon), lx);
            if (t >= 0 && t != g_open.top) {
                openTop(t, mon); // com um menu aberto, passar por outro título já troca (como no macOS)
                return;
            }
        }
        // saiu de dentro do menu: apaga a marca (mexer o mouse fora dele não apaga a marca do teclado)
        if (g_mouseInMenu && !g_open.hover.empty())
            setHover(g_open.hover.size() - 1, -1);
    }
    g_mouseInMenu = inside;
    if (before != std::make_pair(g_open.path, g_open.hover))
        damageAll();
}

// retorna true se o clique deve ser consumido
bool onMouseButton(const IPointer::SButtonEvent& ev) {
    const bool pressed = ev.state == WL_POINTER_BUTTON_STATE_PRESSED;

    if (!pressed) {
        const bool s     = g_swallowRelease;
        g_swallowRelease = false;
        if (g_dragRow >= 0) { // fim do arraste do slider: o menu continua aberto
            g_dragRow     = -1;
            g_pressOpened = false;
            return s;
        }
        // soltar numa linha executa (clique, ou pressionar no título e arrastar até o item)
        if (g_open.top >= 0 && ev.button == BTN_LEFT_) {
            if (auto mon = g_open.mon.lock()) {
                const double lx = g_mouse.x - mon->m_position.x, ly = g_mouse.y - mon->m_position.y;
                const auto   ds = dropdowns(mon);
                for (int k = (int)ds.size() - 1; k >= 0; k--)
                    if (const int r = mb::hitRow(ds[k], lx, ly); r != -2) {
                        if (r >= 0)
                            activateRow(k, r);
                        break;
                    }
            }
        }
        g_pressOpened = false;
        return s;
    }

    const auto mon = monitorAt(g_mouse);
    const bool inBar = mon && !barHidden(mon) && g_mouse.y - mon->m_position.y < barH();
    const int  t     = inBar ? mb::hitTitle(titles(mon), g_mouse.x - mon->m_position.x) : -1;

    if (g_open.top >= 0) {
        g_swallowRelease = true;
        if (auto om = g_open.mon.lock()) {
            const auto   ds = dropdowns(om);
            const double lx = g_mouse.x - om->m_position.x, ly = g_mouse.y - om->m_position.y;
            if (!ds.empty() && ev.button == BTN_LEFT_)
                if (const int r = mb::hitRow(ds[0], lx, ly); r >= 0 && slideTo(ds, r, lx)) {
                    g_dragRow = r; // apertou no slider: muda já e segue o mouse até soltar
                    return true;
                }
            for (auto& d : ds)
                if (mb::hitRow(d, lx, ly) != -2)
                    return true; // no menu: executa ao soltar
        }
        if (t >= 0 && t != g_open.top) {
            openTop(t, mon);
            g_pressOpened = true;
        } else
            closeMenus("clique"); // no mesmo título, na barra vazia ou fora de tudo
        return true;
    }

    if (!inBar)
        return false;
    g_swallowRelease = true;
    if (t >= 0 && ev.button == BTN_LEFT_) {
        openTop(t, mon);
        g_pressOpened = true;
    }
    return true;
}

// ---------- teclado ----------

std::set<uint32_t> g_eaten; // teclas cujo "pressionar" consumimos: o "soltar" também é nosso

// tecla -> código evdev equivalente, pelo keysym do keymap do teclado (teclados virtuais como o do
// wtype usam keymaps próprios, com códigos que não são os do evdev)
uint32_t normalizeKey(uint32_t keycode, const SP<IKeyboard>& kbIn) {
    auto kb = kbIn ? kbIn : g_pSeatManager->m_keyboard.lock();
    if (!kb || !kb->m_xkbState)
        return keycode;
    switch (xkb_state_key_get_one_sym(kb->m_xkbState, keycode + 8)) {
        case XKB_KEY_Escape: return KEY_ESC_;
        case XKB_KEY_Return: return KEY_ENTER_;
        case XKB_KEY_KP_Enter: return KEY_KPENTER_;
        case XKB_KEY_space: return KEY_SPACE_;
        case XKB_KEY_Up: return KEY_UP_;
        case XKB_KEY_Down: return KEY_DOWN_;
        case XKB_KEY_Left: return KEY_LEFT_;
        case XKB_KEY_Right: return KEY_RIGHT_;
        default: return 0; // outra tecla
    }
}

bool onKey(const IKeyboard::SKeyEvent& ev, const SP<IKeyboard>& kb = nullptr) {
    if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return g_eaten.erase(ev.keycode) > 0;
    if (g_open.top < 0)
        return false;
    g_eaten.insert(ev.keycode);

    const size_t   k    = g_open.hover.empty() ? 0 : g_open.hover.size() - 1; // nível mais fundo
    const auto*    menu = levelMenu(k);
    const uint32_t key  = normalizeKey(ev.keycode, kb);
    switch (key) {
        case KEY_ESC_:
            if (g_open.path.empty())
                closeMenus("esc");
            else { // fecha só o submenu
                g_open.path.pop_back();
                g_open.hover.resize(g_open.path.size() + 1);
                damageAll();
            }
            break;
        case KEY_UP_:
        case KEY_DOWN_:
            if (menu) {
                const int r = mb::nextSelectable(*menu, g_open.hover[k], key == KEY_DOWN_ ? 1 : -1);
                // pelo teclado o submenu só abre com → / Enter
                g_open.hover[k] = r;
                damageAll();
            }
            break;
        case KEY_LEFT_:
        case KEY_RIGHT_:
            if (menu && g_open.hover[k] >= 0 && menu->children[g_open.hover[k]].slider) {
                volume::setVolume(volume::state().vol + (key == KEY_RIGHT_ ? VOLUME_STEP : -VOLUME_STEP));
                break;
            }
            if (key == KEY_RIGHT_) {
                if (menu && g_open.hover[k] >= 0 && menu->children[g_open.hover[k]].submenu) {
                    setHover(k, g_open.hover[k]);
                    if (const auto* sub = levelMenu(k + 1); sub && g_open.hover.size() > k + 1)
                        g_open.hover[k + 1] = mb::nextSelectable(*sub, -1, 1);
                    damageAll();
                } else if (auto mon = g_open.mon.lock())
                    openTop((g_open.top + 1) % (int)g_tops.size(), mon);
            } else if (!g_open.path.empty()) {
                g_open.path.pop_back();
                g_open.hover.resize(g_open.path.size() + 1);
                damageAll();
            } else if (auto mon = g_open.mon.lock())
                openTop((g_open.top + (int)g_tops.size() - 1) % (int)g_tops.size(), mon);
            break;
        case KEY_ENTER_:
        case KEY_KPENTER_:
        case KEY_SPACE_:
            if (menu && g_open.hover[k] >= 0) {
                if (menu->children[g_open.hover[k]].submenu) {
                    setHover(k, g_open.hover[k]);
                    if (const auto* sub = levelMenu(k + 1); sub && g_open.hover.size() > k + 1)
                        g_open.hover[k + 1] = mb::nextSelectable(*sub, -1, 1);
                    damageAll();
                } else
                    activateRow(k, g_open.hover[k]);
            }
            break;
        default: break; // com o menu aberto, o resto do teclado também não vai para o app
    }
    return true;
}

// ---------- rolagem ----------
// Rolar sobre o ícone de volume (ou sobre o menu de volume aberto) muda o volume, 5% por clique da roda.

double g_scrollAcc = 0;

bool onAxis(const IPointer::SAxisEvent& e) {
    if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || !volumeShown())
        return false;
    const auto mon = monitorAt(g_mouse);
    if (!mon || barHidden(mon))
        return false;
    const double lx = g_mouse.x - mon->m_position.x, ly = g_mouse.y - mon->m_position.y;
    const auto   ts = titles(mon);
    const int    vi = (int)g_tops.size() - 1; // o volume é sempre o último título
    bool         over = ly >= 0 && ly < barH() && mb::hitTitle(ts, lx) == vi;
    if (!over && g_open.top == vi && g_open.mon.lock() == mon)
        for (auto& d : dropdowns(mon))
            over = over || mb::hitRow(d, lx, ly) != -2;
    if (!over) {
        g_scrollAcc = 0;
        return false;
    }
    g_scrollAcc += e.delta;
    int steps = 0;
    while (g_scrollAcc <= -SCROLL_STEP) { // rolar para cima: aumenta
        g_scrollAcc += SCROLL_STEP;
        steps++;
    }
    while (g_scrollAcc >= SCROLL_STEP) {
        g_scrollAcc -= SCROLL_STEP;
        steps--;
    }
    if (steps)
        volume::setVolume(volume::state().vol + steps * VOLUME_STEP);
    return true;
}

#ifdef HYPRTAB_EVENT_BUS
CHyprSignalListener g_moveListener, g_buttonListener, g_keyListener, g_axisListener;

void registerEvents() {
    auto& ev         = Event::bus()->m_events;
    g_keyListener    = ev.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) {
        if (onKey(e))
            info.cancelled = true;
    });
    g_moveListener   = ev.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo&) { onMouseMove(pos); });
    g_buttonListener = ev.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
        if (onMouseButton(e))
            info.cancelled = true;
    });
    g_axisListener = ev.input.mouse.axis.listen([](IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
        if (onAxis(e))
            info.cancelled = true;
    });
}
void unregisterEvents() {
    g_axisListener.reset();
    g_moveListener.reset();
    g_buttonListener.reset();
    g_keyListener.reset();
}
#else
SP<HOOK_CALLBACK_FN> g_moveCb, g_buttonCb, g_keyCb, g_axisCb;

void registerEvents() {
    g_keyCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "keyPress", [](void*, SCallbackInfo& info, std::any data) {
        try {
            auto map = std::any_cast<std::unordered_map<std::string, std::any>>(data);
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
    g_axisCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseAxis", [](void*, SCallbackInfo& info, std::any data) {
        try {
            auto map = std::any_cast<std::unordered_map<std::string, std::any>>(data);
            if (onAxis(std::any_cast<IPointer::SAxisEvent>(map["event"])))
                info.cancelled = true;
        } catch (...) {}
    });
}
void unregisterEvents() {
    g_axisCb.reset();
    g_moveCb.reset();
    g_buttonCb.reset();
    g_keyCb.reset();
}
#endif

// ---------- abrir pelo teclado ----------
// `hyprmenu:open` abre o menu do app (o nome em negrito) com a primeira linha marcada; `hyprmenu:open N`
// abre o N-ésimo título (0 = logo). Daí as setas navegam, Enter executa, Esc fecha.

void openByKeyboard(const std::string& arg) {
    int t = 1;
    try {
        if (!arg.empty())
            t = std::stoi(arg);
    } catch (...) {}
    rebuild();
    if (g_tops.empty())
        return;
    t       = std::clamp(t, 0, (int)g_tops.size() - 1);
    auto w  = hyprtabFocusedWindow();
    auto mon = w ? w->m_monitor.lock() : nullptr;
    if (!mon)
        mon = monitorAt(g_mouse);
    if (!mon)
        mon = render::currentMonitor();
    if (!mon || barHidden(mon))
        return;
    if (g_open.top == t) {
        closeMenus("toggle");
        return;
    }
    openTop(t, mon);
    if (const auto* m = levelMenu(0))
        g_open.hover[0] = mb::nextSelectable(*m, -1, 1);
    damageAll();
}

SDispatchResult dispOpen(std::string arg) {
    openByKeyboard(arg);
    return {};
}
int luaOpen(auto*) {
    openByKeyboard("");
    return 0;
}

void registerDispatchers() {
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprmenu:open", dispOpen);
    [](auto h) {
        using namespace HyprlandAPI;
        if constexpr (requires { addLuaFunction(h, "hyprmenu", "open", luaOpen); })
            addLuaFunction(h, "hyprmenu", "open", luaOpen);
    }(PHANDLE);
}

SP<SHyprCtlCommand> g_ctl;

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    addConfig();
    loadConfig();
    registerEvents();
    registerDispatchers();
    render::registerRender(draw);

    appmenu::start(g_pCompositor->m_wlEventLoop, onMenusChanged);
    volume::start(g_pCompositor->m_wlEventLoop, onVolumeChanged);
    g_ctl = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{"hyprmenu", true, [](eHyprCtlOutputFormat, std::string) {
                                                                             auto m = g_open.mon.lock();
                                                                             return appmenu::status() + "\n" + volume::status() + "\n" + mapsStatus() + std::format("\nmenu aberto: {} (monitor {}, {} níveis)\ntítulos: {}\n", g_open.top, m ? m->m_name : "-", g_open.hover.size(), g_tops.size()) + "último fechamento: " + g_closeWhy + "\n";
                                                                         }});

    g_pollTimer       = makeShared<CEventLoopTimer>(std::nullopt, onPoll, nullptr);
    g_focusTimer = makeShared<CEventLoopTimer>(std::nullopt, onFocusTimer, nullptr);
    g_soon       = makeShared<CEventLoopTimer>(std::nullopt, onSoon, nullptr);
    for (auto& t : {g_pollTimer, g_focusTimer, g_soon})
        g_pEventLoopManager->addTimer(t);

    updateClock();
    loadMaps();
    rebuild();
    g_pollTimer->updateTimeout(POLL_EVERY);
    g_focusTimer->updateTimeout(FOCUS_EVERY);

    installReserve();
    syncReserve();
    damageAll();

    return {"hyprmenu", "Barra de menus global", "caio", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    for (auto* t : {&g_pollTimer, &g_focusTimer, &g_soon})
        if (*t) {
            g_pEventLoopManager->removeTimer(*t);
            t->reset();
        }
    unregisterEvents();
    render::unregisterRender();
    if (g_ctl)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_ctl);
    appmenu::stop();
    volume::stop();
    g_open = {};
    g_tops.clear();
    removeReserve();
    damageAll();
}
