// hyprdock - dock inspirada no macOS para o Hyprland, tudo dentro do .so.
// Usa o código comum do hyprtab (../src): render (painel de vidro, por era), ícones, compat e o
// parser do `hyprctl clients -j`.
//
// Config (hyprlang, até 0.56):
//   plugin {
//       hyprdock {
//           apps = google-chrome, Alacritty   # fixos, por classe/id do .desktop
//           icon_size = 48
//           margin = 8                       # distância da borda de baixo
//           show_running = 1                 # mostra também os apps abertos não fixos
//           magnification = 1.6              # zoom dos ícones sob o mouse; 1 desliga
//           position = bottom                # bottom, left ou right
//           reserve = 1                      # reserva a faixa da dock: janelas não ficam por baixo
//           ignore = steam_app_*, foo        # classes que não aparecem como abertas (além das padrão)
//       }
//   }
//
// Botão direito num app abre o menu: fechar, maximizar, minimizar, fixar/remover da dock.
// Fixar/remover pelo menu fica em $XDG_STATE_HOME/hyprdock/pins (ou ~/.local/state/...), por cima do `apps`.
// Minimizar manda a janela para o workspace especial "minimized"; clicar no app traz de volta.

#include "../../src/clients.hpp"
#include "../../src/icons.hpp"
#include "../../src/render.hpp"
#include "model.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#if __has_include(<hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>)
#include <hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>
#endif

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

static constexpr uint32_t BTN_LEFT_  = 0x110;
static constexpr uint32_t BTN_RIGHT_ = 0x111;
static constexpr uint32_t KEY_ESC_   = 1;

// lista de janelas e config são relidas nesse intervalo (independe de eventos, que mudam por versão)
static constexpr auto POLL_EVERY = std::chrono::milliseconds(500);
// animação do zoom: um passo por quadro (~60 Hz), entrada/saída completa nesse tempo
static constexpr auto   ANIM_FRAME = std::chrono::milliseconds(16);
static constexpr double ANIM_MS    = 110;

namespace {

constexpr double RADIUS = 18, HOVER_RADIUS = 12, DOT_R = 2.5, TIP_H = 26, TIP_PADX = 12, TIP_GAP = 8;
constexpr double SEP_LINE_H = 0.8; // altura da linha do separador, em fração do ícone

std::vector<dock::Item>  g_items;
std::vector<std::string> g_order; // ordem dos apps abertos não fixos
int                      g_hover = -1;
Vector2D                 g_mouse = {};
bool                     g_swallowRelease = false;
SP<CEventLoopTimer>      g_poll;

// zoom: só no monitor onde está o mouse; `g_level` anima de 0 a 1 ao entrar e volta ao sair
PHLMONITORREF       g_magMon;
double              g_magX     = 0; // x do mouse, local ao monitor (o último dentro da dock)
double              g_level    = 0;
double              g_levelTo  = 0;
SP<CEventLoopTimer> g_anim;

// arrastar para reordenar os fixos: pressionar e mover mais que DRAG_START px; soltar sem mover é clique
constexpr double DRAG_START = 6, DRAG_LIFT = 4, DETACHED_ALPHA = 0.55;
struct Drag {
    bool          pressed = false, active = false;
    std::string   cls;      // item pressionado
    size_t        from = 0; // índice dele na dock
    size_t        drop = 0; // onde cairia agora
    PHLMONITORREF mon;
    Vector2D      start;    // onde o botão foi pressionado (global)
    double        x = 0;    // cursor ao longo da fileira, no espaço da dock
    bool          outside = false; // longe da dock: soltar remove dos fixos
    Vector2D      local;           // cursor no monitor (px lógicos), para desenhar o ícone solto
} g_drag;

// redimensionar arrastando o separador / área vazia (ver "redimensionar" mais abaixo)
struct Resize {
    bool          pressed = false, active = false;
    PHLMONITORREF mon;
    Vector2D      start;          // onde o botão foi pressionado (global)
    double        startDy = 0;    // no espaço da dock (y cresce em direção à borda)
    double        startSize = 48; // tamanho do ícone no começo
    double        want = 0;       // tamanho pedido pelo mouse; aplicado no próximo quadro (onAnim)
    double        startIcon = 0;  // ícone e espessura do painel no começo (px lógicos): enquanto arrasta,
    double        startPanel = 0; // texturas e sombra dessas medidas são só esticadas (nada é gerado)
} g_resize;

// entrada: a dock desliza de fora da borda ao carregar e ao trocar de borda (ver dock::enterOffset)
bool                                  g_entering     = false;
bool                                  g_enterPending = false; // o relógio só começa no primeiro quadro desenhado
std::chrono::steady_clock::time_point g_enterStart;

// puffs de fumaça de ícones removidos arrastando para fora (ver dock::poofPuffs)
struct Poof {
    PHLMONITORREF                         mon;
    Vector2D                              at; // ponto no monitor (px lógicos)
    std::string                           cls;
    std::chrono::steady_clock::time_point start;
};
std::vector<Poof> g_poofs;

// textura de uma partícula de fumaça: blobs com degradê radial sobrepostos (contorno irregular, bordas
// esfumadas). variant = formato (0..2) + 3 * tom (claro / acinzentado); -1 = clarão (um só blob, branco).
// Geradas uma vez e só escaladas ao desenhar.
render::Tex smokeTexture(int variant) {
    static auto* cache = new std::unordered_map<int, render::Tex>();
    if (auto it = cache->find(variant); it != cache->end())
        return it->second;

    constexpr int    S    = 128;
    const double     tone = variant < 0 ? 1.0 : variant / 3 == 0 ? 0.96 : 0.74;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, S, S);
    cairo_t*         cr   = cairo_create(surf);
    auto blob = [&](double cx, double cy, double r, double a) {
        auto* g = cairo_pattern_create_radial(cx, cy, 0, cx, cy, r);
        cairo_pattern_add_color_stop_rgba(g, 0, tone, tone, tone, a);
        cairo_pattern_add_color_stop_rgba(g, 0.45, tone, tone, tone, a * 0.6);
        cairo_pattern_add_color_stop_rgba(g, 1, tone, tone, tone, 0);
        cairo_set_source(cr, g);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    };
    if (variant < 0)
        blob(S / 2.0, S / 2.0, S / 2.0, 0.9);
    else {
        const int shape = variant % 3;
        blob(S / 2.0, S / 2.0, S * 0.36, 0.5); // corpo
        for (int k = 0; k < 5; k++) {           // lóbulos em volta, em posições fixas por formato
            const double a = 2 * M_PI * (k + 0.37 * shape) / 5 + shape;
            const double d = S * (0.10 + 0.04 * ((k + shape) % 3));
            blob(S / 2.0 + std::cos(a) * d, S / 2.0 + std::sin(a) * d, S * (0.20 + 0.03 * ((k * 2 + shape) % 4)), 0.35);
        }
    }
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    auto t = render::makeTexture(surf);
    cairo_surface_destroy(surf);
    return (*cache)[variant] = t;
}

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// apps abrindo pela dock: o ícone pula até aparecer uma janela (ou estourar o tempo)
struct Bounce {
    std::chrono::steady_clock::time_point start;
    double                                landAt = -1; // ms desde o início; -1: ainda pulando
};
std::unordered_map<std::string, Bounce> g_bouncing; // por classe do item

double elapsedMs(const Bounce& b) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b.start).count();
}

void kickAnim();
void startEnter();
void syncReserve();
void animateTo(double target);
bool updateZoom(const Vector2D& pos);

// onde a dock foi desenhada em cada monitor, para o mouse (a lista de monitores mudou de lugar entre versões)
struct Placed {
    PHLMONITORREF mon;
    dock::Geom    g; // no espaço da dock (ver model.hpp: sempre "embaixo")
    dock::Frame   f; // converte entre o espaço da dock e o monitor
};
std::vector<Placed> g_placed;

dock::PinOverrides    g_pins;      // fixar/remover feito pelo menu (persistido)
std::set<std::string> g_minimized; // endereços que minimizamos (estão em special:minimized)

// ---------- menu de contexto ----------

// menu de um app (clique direito no ícone) ou da dock (clique direito no separador / área vazia)
enum class Action {
    Open, Close, Maximize, Minimize, Pin, Unpin,                                                 // app
    Magnify, MagLevel, Reserve, ShowRunning, Position, PosLeft, PosBottom, PosRight, Separator, // dock
    TrashOpen, TrashEmpty, TrashConfirm, TrashCancel,                                           // Lixeira
};

struct Menu {
    bool                open = false;
    bool                dock = false; // menu da dock (não de um app)
    double              at   = 0;     // menu da dock: onde foi o clique, ao longo da fileira (espaço da dock)
    std::string         cls; // por classe: os índices mudam quando a lista é relida
    PHLMONITORREF       mon;
    std::vector<Action> rows;
    int                 hover = -1;
    dock::MenuGeom      g     = {}; // local ao monitor, atualizado no draw
    bool                drawn = false;
    // submenu (Posição na Tela): aberto ao passar o mouse na linha `subRow`
    int                 subRow   = -1; // linha do menu que abriu o submenu; -1: fechado
    std::vector<Action> subRows;
    int                 subHover = -1;
    dock::MenuGeom      sg       = {};
} g_menu;

constexpr double MENU_RADIUS = 12, ROW_RADIUS = 8, ROW_TEXT_X = 14, CHECK_W = 16, ARROW_W = 14;

// ---------- config ----------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations" // 0.57 marca a API hyprlang como deprecated

void addConfig() {
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprdock:apps", Hyprlang::STRING{""});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprdock:icon_size", Hyprlang::INT{48});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprdock:margin", Hyprlang::INT{8});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprdock:show_running", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprdock:magnification", Hyprlang::FLOAT{1.6F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprdock:position", Hyprlang::STRING{"bottom"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprdock:reserve", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprdock:ignore", Hyprlang::STRING{""});
}

// com config Lua (0.57+) o valor pode não existir: cai no padrão
Hyprlang::INT cfgInt(const char* name, Hyprlang::INT def) {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, name);
    return v ? **(Hyprlang::INT* const*)v->getDataStaticPtr() : def;
}
Hyprlang::FLOAT cfgFloat(const char* name, Hyprlang::FLOAT def) {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, name);
    return v ? **(Hyprlang::FLOAT* const*)v->getDataStaticPtr() : def;
}
std::string cfgStr(const char* name) {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, name);
    if (!v)
        return "";
    auto s = *(Hyprlang::STRING const*)v->getDataStaticPtr();
    return s ? s : "";
}

#pragma GCC diagnostic pop

// config lida uma vez por ciclo de leitura (500 ms), não a cada quadro: buscar por nome é caro
struct Config {
    std::vector<std::string> apps;
    std::vector<std::string> ignore; // padrão + config
    dock::Edge               edge        = dock::Edge::Bottom;
    double                   iconSize    = 48, margin = 8, mag = 1.6;
    double                   magLevel    = 1.6; // valor da ampliação, mesmo desligada (o controle do menu mostra)
    bool                     showRunning = true, reserve = true;
} g_cfg;

// ---------- ajustes feitos pelo menu da dock (persistidos) ----------
// Ficam em $XDG_STATE_HOME/hyprdock/settings (ou ~/.local/state/...), por cima da config: uma linha
// `chave=valor` por ajuste mudado. Apagar o arquivo volta ao que está no hyprland.conf.

struct Settings {
    std::optional<dock::Edge> edge;
    std::optional<bool>       magnify, reserve, showRunning;
    std::optional<double>     iconSize; // arrastando o separador
    std::optional<double>     magLevel; // controle de ampliação do menu
} g_set;

constexpr double ICON_MIN = 16, ICON_MAX = 128;

std::filesystem::path statePath(const char* name) {
    if (const char* x = getenv("XDG_STATE_HOME"); x && *x)
        return std::filesystem::path(x) / "hyprdock" / name;
    if (const char* h = getenv("HOME"))
        return std::filesystem::path(h) / ".local/state/hyprdock" / name;
    return {};
}

void loadSettings() {
    g_set = {};
    std::ifstream f(statePath("settings"));
    std::string   line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const auto key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "position")
            g_set.edge = dock::parseEdge(val);
        else if (key == "magnification_level") {
            try {
                g_set.magLevel = std::clamp(std::stod(val), dock::MAG_MIN, dock::MAG_MAX);
            } catch (...) {}
        } else if (key == "magnification")
            g_set.magnify = val == "1";
        else if (key == "reserve")
            g_set.reserve = val == "1";
        else if (key == "show_running")
            g_set.showRunning = val == "1";
        else if (key == "icon_size") {
            try {
                g_set.iconSize = std::clamp(std::stod(val), ICON_MIN, ICON_MAX);
            } catch (...) {}
        }
    }
}

void saveSettings() {
    const auto      path = statePath("settings");
    std::error_code ec;
    if (path.empty())
        return;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (g_set.edge)
        out << "position=" << (*g_set.edge == dock::Edge::Left ? "left" : *g_set.edge == dock::Edge::Right ? "right" : "bottom") << "\n";
    if (g_set.magnify)
        out << "magnification=" << (*g_set.magnify ? 1 : 0) << "\n";
    if (g_set.magLevel)
        out << std::format("magnification_level={:.2f}\n", *g_set.magLevel);
    if (g_set.reserve)
        out << "reserve=" << (*g_set.reserve ? 1 : 0) << "\n";
    if (g_set.showRunning)
        out << "show_running=" << (*g_set.showRunning ? 1 : 0) << "\n";
    if (g_set.iconSize)
        out << "icon_size=" << (int)std::lround(*g_set.iconSize) << "\n";
}

// devolve true se a borda mudou (a dock sai de um lugar e aparece em outro)
bool loadConfig() {
    const auto old = g_cfg.edge;
    g_cfg.apps        = splitList(cfgStr("plugin:hyprdock:apps"));
    g_cfg.ignore      = DEFAULT_IGNORE;
    for (auto& p : splitList(cfgStr("plugin:hyprdock:ignore")))
        g_cfg.ignore.push_back(p);
    g_cfg.edge        = dock::parseEdge(cfgStr("plugin:hyprdock:position"));
    g_cfg.iconSize    = (double)std::max<Hyprlang::INT>(8, cfgInt("plugin:hyprdock:icon_size", 48));
    g_cfg.margin      = (double)cfgInt("plugin:hyprdock:margin", 8);
    g_cfg.mag         = std::max(1.0, (double)cfgFloat("plugin:hyprdock:magnification", 1.6F));
    g_cfg.showRunning = cfgInt("plugin:hyprdock:show_running", 1);
    g_cfg.reserve     = cfgInt("plugin:hyprdock:reserve", 1);
    // ajustes do menu da dock por cima
    if (g_set.edge)
        g_cfg.edge = *g_set.edge;
    // ampliação: ligada/desligada e valor vêm separados (o menu muda cada um); config em 1 = desligada
    g_cfg.magLevel = g_set.magLevel ? *g_set.magLevel : g_cfg.mag > 1 ? g_cfg.mag : dock::MAG_DEFAULT;
    if (g_set.magnify ? *g_set.magnify : g_cfg.mag > 1)
        g_cfg.mag = g_cfg.magLevel;
    else
        g_cfg.mag = 1;
    if (g_set.reserve)
        g_cfg.reserve = *g_set.reserve;
    if (g_set.showRunning)
        g_cfg.showRunning = *g_set.showRunning;
    if (g_set.iconSize)
        g_cfg.iconSize = *g_set.iconSize;
    return g_cfg.edge != old;
}

// ---------- fixos persistidos ----------

std::filesystem::path pinsPath() {
    return statePath("pins");
}

void loadPins() {
    std::ifstream f(pinsPath());
    std::string   text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    g_pins = dock::parsePins(text);
}

void savePins() {
    const auto      path = pinsPath();
    std::error_code ec;
    if (path.empty())
        return;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream(path) << dock::serializePins(g_pins);
}

const std::vector<std::string>& configPins() {
    return g_cfg.apps;
}

// ---------- ações ----------

void damageDock(const PHLMONITOR& mon);

void damageAll() {
    for (auto& p : g_placed)
        if (auto m = p.mon.lock())
            damageDock(m);
    if (auto m = render::currentMonitor())
        g_pHyprRenderer->damageMonitor(m);
}

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

std::string focusedAddr() {
    const auto f = hyprtabFocusedWindow();
    return f ? hyprtabAddr(f) : "";
}

// id do workspace ativo, de `hyprctl activeworkspace -j` (formato estável entre versões)
std::string activeWorkspaceId() {
    const auto js = HyprlandAPI::invokeHyprctlCommand("activeworkspace", "", "j");
    const auto at = js.find("\"id\":");
    if (at == std::string::npos)
        return "1";
    size_t i = at + 5;
    while (i < js.size() && js[i] == ' ')
        i++;
    size_t j = i;
    while (j < js.size() && (js[j] == '-' || isdigit((unsigned char)js[j])))
        j++;
    return j > i ? js.substr(i, j - i) : "1";
}

void moveSilent(const std::string& addr, const std::string& ws) {
    const bool numeric = !ws.empty() && (isdigit((unsigned char)ws[0]) || ws[0] == '-');
    hyprtabDispatch(std::format("movetoworkspacesilent {},address:{}", ws, addr),
                    std::format("hl.dsp.window.move({{ workspace = {}, follow = false, window = 'address:{}' }})", numeric ? ws : "'" + ws + "'", addr));
}

void restore(const std::string& addr) {
    moveSilent(addr, activeWorkspaceId());
    g_minimized.erase(addr);
}

void closeWindow(const std::string& addr) {
    hyprtabDispatch(std::format("closewindow address:{}", addr), std::format("hl.dsp.window.close({{ window = 'address:{}' }})", addr));
}

void maximize(const std::string& addr) {
    if (g_minimized.contains(addr))
        restore(addr);
    hyprtabFocus(addr);
    // hyprlang: `fullscreen 1` age na janela focada (por isso o foco antes)
    hyprtabDispatch("fullscreen 1", std::format("hl.dsp.window.fullscreen({{ mode = 'maximized', window = 'address:{}' }})", addr));
}

void minimize(const std::string& addr) {
    moveSilent(addr, "special:minimized");
    g_minimized.insert(addr);
}

// ---------- Lixeira ----------
// A pasta da lixeira do usuário (especificação freedesktop): ~/.local/share/Trash/files. O ícone fica
// "cheio" quando ela tem alguma coisa. Abrir e esvaziar vão pelo gio (o mesmo que o Arquivos usa).

const char* TRASH_CLS = "user-trash"; // classe do item da Lixeira (nome do ícone vazio)
bool        g_trashFull = false;

std::filesystem::path trashFiles() {
    if (const char* x = getenv("XDG_DATA_HOME"); x && *x)
        return std::filesystem::path(x) / "Trash/files";
    if (const char* h = getenv("HOME"))
        return std::filesystem::path(h) / ".local/share/Trash/files";
    return {};
}

bool trashHasFiles() {
    std::error_code ec;
    std::filesystem::directory_iterator it(trashFiles(), ec);
    return !ec && it != std::filesystem::directory_iterator();
}

// nome do ícone do item (a Lixeira muda com o conteúdo)
std::string iconName(const dock::Item& it) {
    return it.trash ? (g_trashFull ? "user-trash-full" : "user-trash") : it.cls;
}

void activate(const dock::Item& it) {
    if (it.trash) {
        // pede ao gerenciador de arquivos (serviço freedesktop FileManager1, que o Arquivos implementa):
        // o `gio open` usaria o app padrão para pastas, que pode ser outro programa (uma IDE, por exemplo)
        spawn("gdbus call --session --dest org.freedesktop.FileManager1 --object-path /org/freedesktop/FileManager1 "
              "--method org.freedesktop.FileManager1.ShowFolders \"['trash:///']\" '' >/dev/null 2>&1 "
              "|| nautilus trash:/// || gio open trash:///");
        return;
    }
    if (const auto addr = dock::nextWindow(it, focusedAddr()); !addr.empty()) {
        if (g_minimized.contains(addr))
            restore(addr);
        hyprtabFocus(addr);
        return;
    }
    // não está aberto: abre pelo Exec= do .desktop, ou pelo próprio nome
    const auto* de  = icons::desktopEntry(it.cls);
    const auto  cmd = de && !de->exec.empty() ? dock::stripFieldCodes(de->exec) : it.cls;
    spawn(cmd);
    g_bouncing[it.cls] = {std::chrono::steady_clock::now(), -1};
    kickAnim();
}

std::string displayName(const dock::Item& it) {
    if (it.trash)
        return "Lixeira";
    const auto* de = icons::desktopEntry(it.cls);
    return de && !de->name.empty() ? de->name : it.cls;
}

// ---------- estado ----------

void refresh() {
    std::vector<std::pair<std::string, std::string>> windows;
    for (auto& [addr, ct] : parseClients(HyprlandAPI::invokeHyprctlCommand("clients", "", "j")))
        if (!classIgnored(ct.first, g_cfg.ignore)) // janelas auxiliares não viram "app aberto"
            windows.emplace_back(addr, ct.first);

    auto items = dock::buildItems(dock::effectivePins(configPins(), g_pins), windows, g_order, g_cfg.showRunning);
    items.push_back({TRASH_CLS, false, {}, true}); // a Lixeira, sempre no fim
    if (const bool full = trashHasFiles(); full != g_trashFull) {
        g_trashFull = full;
        damageAll();
    }

    std::erase_if(g_minimized, [&](const std::string& a) { return std::none_of(windows.begin(), windows.end(), [&](auto& w) { return w.first == a; }); });

    const bool changed = items.size() != g_items.size() || !std::equal(items.begin(), items.end(), g_items.begin(), [](const dock::Item& a, const dock::Item& b) {
                             return a.cls == b.cls && a.windows.empty() == b.windows.empty();
                         });
    // app aberto por fora da dock (lançador, terminal...): pula BOUNCE_APPEAR vezes quando a janela aparece.
    // Não na primeira leitura (apps que já estavam abertos ao carregar o plugin) nem se já está pulando.
    static bool first = true;
    if (!first)
        for (auto& cls : dock::newlyOpened(g_items, items))
            if (!g_bouncing.contains(cls)) {
                g_bouncing[cls] = {std::chrono::steady_clock::now(), dock::BOUNCE_APPEAR * dock::BOUNCE_PERIOD};
                kickAnim();
            }
    first = false;

    g_items = std::move(items);
    for (auto& [cls, b] : g_bouncing) {
        if (b.landAt >= 0)
            continue;
        const auto it     = std::find_if(g_items.begin(), g_items.end(), [&](const dock::Item& i) { return i.cls == cls; });
        const bool opened = it == g_items.end() || !it->windows.empty();
        if (opened || elapsedMs(b) > dock::BOUNCE_TIMEOUT)
            b.landAt = dock::bounceLanding(elapsedMs(b)); // termina o pulo e pousa
    }
    if (g_menu.open && !g_menu.dock && std::none_of(g_items.begin(), g_items.end(), [](const dock::Item& i) { return i.cls == g_menu.cls; }))
        g_menu.open = false; // o app sumiu da dock (fechou e não é fixo)
    if (changed) {
        g_hover = -1;
        damageAll();
    }
}

// ---------- menu: conteúdo e ações ----------

const dock::Item* menuItem(int* index = nullptr) {
    for (size_t i = 0; i < g_items.size(); i++)
        if (g_items[i].cls == g_menu.cls) {
            if (index)
                *index = (int)i;
            return &g_items[i];
        }
    return nullptr;
}

std::vector<Action> menuRows(const dock::Item& it) {
    if (it.trash)
        return g_trashFull ? std::vector<Action>{Action::TrashOpen, Action::TrashEmpty} : std::vector<Action>{Action::TrashOpen};
    std::vector<Action> rows;
    if (it.windows.empty())
        rows.push_back(Action::Open);
    else
        rows.insert(rows.end(), {Action::Close, Action::Maximize, Action::Minimize});
    rows.push_back(it.pinned ? Action::Unpin : Action::Pin);
    return rows;
}

std::vector<Action> dockMenuRows() {
    return {Action::Magnify, Action::MagLevel, Action::Reserve, Action::ShowRunning, Action::Separator, Action::Position};
}

// `it`: o app do menu (nullptr no menu da dock)
std::string label(Action a, const dock::Item* it) {
    switch (a) {
        case Action::Open: return "Abrir";
        case Action::Close: return it && it->windows.size() > 1 ? std::format("Fechar todas ({})", it->windows.size()) : "Fechar";
        case Action::Maximize: return "Maximizar";
        case Action::Minimize: return "Minimizar";
        case Action::Pin: return "Fixar na Dock";
        case Action::Unpin: return "Remover da Dock";
        case Action::Magnify: return "Ampliação";
        case Action::Reserve: return "Reservar Espaço na Tela";
        case Action::ShowRunning: return "Mostrar Apps Abertos";
        case Action::Position: return "Posição na Tela";
        case Action::PosLeft: return "Esquerda";
        case Action::PosBottom: return "Embaixo";
        case Action::PosRight: return "Direita";
        case Action::TrashOpen: return "Abrir";
        case Action::TrashEmpty: return "Esvaziar Lixeira…";
        case Action::TrashConfirm: return "Apagar Itens Permanentemente";
        case Action::TrashCancel: return "Cancelar";
        case Action::MagLevel:
        case Action::Separator: return "";
    }
    return "";
}

// linha com marca (✓) ligada
bool checked(Action a) {
    switch (a) {
        case Action::Magnify: return g_cfg.mag > 1;
        case Action::Reserve: return g_cfg.reserve;
        case Action::ShowRunning: return g_cfg.showRunning;
        case Action::PosLeft: return g_cfg.edge == dock::Edge::Left;
        case Action::PosBottom: return g_cfg.edge == dock::Edge::Bottom;
        case Action::PosRight: return g_cfg.edge == dock::Edge::Right;
        default: return false;
    }
}

std::vector<bool> menuSeps() {
    std::vector<bool> seps;
    for (auto a : g_menu.rows)
        seps.push_back(a == Action::Separator);
    return seps;
}

// ---------- controle de ampliação no menu ----------

constexpr double SLIDER_VALUE_W = 40, SLIDER_KNOB = 14, SLIDER_TRACK_H = 4;

bool g_slider = false; // arrastando o controle

// trilho do controle (local ao monitor), na linha MagLevel do menu
void sliderTrack(double& x0, double& x1) {
    x0 = g_menu.g.x + ROW_TEXT_X + CHECK_W + SLIDER_KNOB / 2;
    x1 = g_menu.g.x + g_menu.g.w - ROW_TEXT_X - SLIDER_VALUE_W - SLIDER_KNOB / 2;
}

void damageAll();

void setMagLevel(double v) {
    v = std::round(v * 10) / 10; // passos de 0,1
    if (v == g_cfg.magLevel)
        return;
    g_set.magLevel = v;
    g_cfg.magLevel = v;
    if (g_cfg.mag > 1)
        g_cfg.mag = v;
    damageAll();
}

void sliderFromMouse(const Vector2D& pos) {
    auto mon = g_menu.mon.lock();
    if (!mon)
        return;
    double x0 = 0, x1 = 0;
    sliderTrack(x0, x1);
    setMagLevel(dock::sliderValue(pos.x - mon->m_position.x, x0, x1, dock::MAG_MIN, dock::MAG_MAX));
}

// aplica um ajuste do menu da dock: grava e relê a config (com a mesma entrada pela borda nova do poll)
void runDock(Action a) {
    switch (a) {
        case Action::Magnify: g_set.magnify = !(g_cfg.mag > 1); break;
        case Action::Reserve: g_set.reserve = !g_cfg.reserve; break;
        case Action::ShowRunning: g_set.showRunning = !g_cfg.showRunning; break;
        case Action::PosLeft: g_set.edge = dock::Edge::Left; break;
        case Action::PosBottom: g_set.edge = dock::Edge::Bottom; break;
        case Action::PosRight: g_set.edge = dock::Edge::Right; break;
        default: return;
    }
    saveSettings();
    if (loadConfig()) {
        startEnter();
        for (auto& p : g_placed)
            if (auto m = p.mon.lock())
                g_pHyprRenderer->damageMonitor(m);
    }
    refresh();
    syncReserve();
    damageAll();
}

void run(Action a, const dock::Item& it) {
    const auto target = dock::targetWindow(it, focusedAddr());
    switch (a) {
        case Action::TrashOpen: activate(it); break;
        case Action::TrashEmpty: // apaga para sempre: pede confirmação no próprio menu
            g_menu.open = true;
            g_menu.rows = {Action::TrashConfirm, Action::TrashCancel};
            g_menu.hover = -1;
            damageAll();
            break;
        case Action::TrashConfirm:
            spawn("gio trash --empty");
            g_trashFull = false; // o ícone volta já; a próxima leitura confirma
            damageAll();
            break;
        case Action::TrashCancel: break;
        default: break; // ações do menu da dock: runDock
        case Action::Open: activate(it); break;
        case Action::Close:
            for (auto& w : it.windows)
                closeWindow(w);
            break;
        case Action::Maximize:
            if (!target.empty())
                maximize(target);
            break;
        case Action::Minimize:
            if (!target.empty())
                minimize(target);
            break;
        case Action::Pin:
        case Action::Unpin:
            dock::setPinned(g_pins, configPins(), it.cls, a == Action::Pin);
            savePins();
            refresh();
            break;
    }
}

void openMenu(size_t i, const PHLMONITOR& mon) {
    g_menu      = {};
    g_menu.open = true;
    g_menu.cls  = g_items[i].cls;
    g_menu.mon  = mon;
    g_menu.rows = menuRows(g_items[i]);
    damageAll(); // o zoom fica congelado como está (ver onMouseMove); o menu ancora no item ampliado
}

// menu da dock, ancorado onde foi o clique (`at`: ao longo da fileira, no espaço da dock)
void openDockMenu(const PHLMONITOR& mon, double at) {
    g_menu      = {};
    g_menu.open = true;
    g_menu.dock = true;
    g_menu.at   = at;
    g_menu.mon  = mon;
    g_menu.rows = dockMenuRows();
    damageAll(); // o zoom fica congelado como está (ver onMouseMove); o menu ancora no item ampliado
}

void closeMenu() {
    if (!g_menu.open)
        return;
    g_menu.open = false;
    if (g_slider) { // fechou no meio do arraste do controle (Esc): o valor fica
        g_slider = false;
        saveSettings();
    }
    animateTo(updateZoom(g_mouse) ? 1 : 0); // descongela: volta a seguir o mouse (ou encolhe, se saiu)
    damageAll();
}

void onPoll(SP<CEventLoopTimer> self, void*) {
    // mudou de borda: a dock sai de um lugar e aparece em outro, redesenha o monitor todo
    if (loadConfig()) {
        startEnter(); // entra pela borda nova
        for (auto& p : g_placed)
            if (auto m = p.mon.lock())
                g_pHyprRenderer->damageMonitor(m);
    }
    refresh();
    syncReserve();
    self->updateTimeout(POLL_EVERY);
}

dock::Frame frameFor(const PHLMONITOR& mon) {
    return {g_cfg.edge, mon->m_size.x, mon->m_size.y};
}

size_t pinnedCount();

// `pinned`: quantos dos `n` itens são fixos. Separadores depois dos fixos (se há abertos não fixos) e
// antes da Lixeira (o último); quando coincidem, um só.
dock::Geom geometry(const dock::Frame& f, size_t n, size_t pinned) {
    return dock::geometry(n, f.w(), f.h(), g_cfg.iconSize, g_cfg.margin, {pinned, n > 0 ? n - 1 : 0});
}
dock::Geom geometry(const dock::Frame& f) {
    return geometry(f, g_items.size(), pinnedCount());
}

// ---------- desenho ----------

double magnification() {
    return g_cfg.mag;
}

// itens como são desenhados neste monitor: ampliados se o mouse está nele, normais nos outros
dock::Layout layoutFor(const PHLMONITOR& mon, const dock::Geom& g) {
    const bool here = !g_drag.active && g_level > 0 && g_magMon.lock() == mon; // arrastando: sem zoom
    return dock::magnify(g, g_items.size(), g_magX, here ? magnification() : 1, here ? g_level : 0);
}

void draw(const PHLMONITOR& mon) {
    std::erase_if(g_placed, [&](const Placed& p) { return p.mon.expired() || p.mon.lock() == mon; });
    if (g_items.empty() || hyprtabLocked() || hyprtabHasFullscreen(mon))
        return;

    const double sc  = mon->m_scale;
    const auto   f   = frameFor(mon);
    const auto   g   = geometry(f);
    auto         l   = layoutFor(mon, g);
    const double mag = magnification();
    const size_t n   = g_items.size();
    const double cs  = g.icon + 2 * dock::PAD * g.k; // célula sem zoom
    auto         px  = [&](double v) { return v * sc; };
    // textura no tamanho máximo do zoom; nos tamanhos intermediários a GPU só reduz
    // em degraus de 16 px (sempre para cima: a GPU só reduz): redimensionando a dock, cada tamanho
    // novo não vira uma textura nova no cache
    // redimensionando: o tamanho do começo (só esticado), para não carregar ícones a cada passo
    const double texIcon = g_resize.active && g_resize.startIcon > 0 ? g_resize.startIcon : g.icon;
    const int    texPx   = std::max(16, (int)std::ceil(px(texIcon * mag) / 16) * 16);
    // entrando: o primeiro quadro carrega os ícones (disco, pode levar centenas de ms) antes de o relógio
    // começar; senão a animação já estaria no fim quando a dock aparecesse
    if (g_enterPending) {
        for (auto& it : g_items)
            render::iconTexture(iconName(it), texPx);
        g_enterStart   = std::chrono::steady_clock::now();
        g_enterPending = false;
    }
    // entrando: tudo deslocado para fora da borda (com folga para a sombra), voltando ao lugar
    const double enter = g_entering ? dock::enterOffset(msSince(g_enterStart), f.h() - g.y + 64 * g.k) : 0;
    // caixa no espaço da dock -> caixa no monitor (px físicos)
    auto scr = [&](double x, double y, double w, double h) {
        const auto b = dock::toScreen(f, {x, y + enter, w, h});
        return CBox{px(b.x), px(b.y), px(b.w), px(b.h)};
    };
    g_placed.push_back({mon, g, f});

    const bool hoverHere = g_hover >= 0 && mon->logicalBox().containsPoint(g_mouse);

    // arrastando neste monitor: os outros itens abrem espaço em `drop`; o arrastado vai por último, por cima.
    // Solto (longe da dock): a dock já se mostra sem ele, que segue o cursor livre e meio transparente.
    const bool dragging = g_drag.active && g_drag.mon.lock() == mon && g_drag.from < n;
    const bool detached = dragging && g_drag.outside;
    if (detached)
        l = dock::magnify(geometry(f, n - 1, pinnedCount() - (g_items[g_drag.from].pinned ? 1 : 0)), n - 1, 0, 1, 0);
    auto slot = [&](size_t i) { return !dragging ? i : detached ? (i > g_drag.from ? i - 1 : i) : dock::dragSlot(i, g_drag.from, g_drag.drop); };

    if (!(detached && n == 1)) // o único item saiu: não sobra dock
        render::glassPanel(scr(l.panel.x, l.panel.y, l.panel.w, l.panel.h), (int)px(RADIUS * g.k), sc, g.k,
                           g_resize.active && g_resize.startPanel > 0 ? (int)std::lround(px(g_resize.startPanel)) : 0);

    // separador entre fixos e abertos: linha fina na altura dos ícones, no meio do vão
    for (const double sx : l.sepX) {
        const double lw = std::max(1.0, std::round(sc)) / sc; // 1 px físico (em px lógicos)
        const double lh = g.icon * SEP_LINE_H;
        const double ly = g.y + dock::BARPAD * g.k + (g.icon + 2 * dock::PAD * g.k - lh) / 2;
        render::fillRect(scr(sx - lw / 2, ly, lw, lh), CHyprColor{1, 1, 1, 0.32}, 0);
    }

    for (size_t i = 0; i < n; i++) {
        if (dragging && i == g_drag.from)
            continue;
        const auto& c = l.cells[slot(i)];

        // com zoom ligado o próprio tamanho já destaca o item; o fundo claro fica só sem zoom
        if (hoverHere && !dragging && (size_t)g_hover == i && mag <= 1)
            render::fillRect(scr(c.x, c.y, c.size, c.size), CHyprColor{1, 1, 1, 0.16}, (int)px(HOVER_RADIUS * g.k));

        const double icon = c.size * (g.icon / cs); // o padding cresce junto com o ícone
        const double off  = (c.size - icon) / 2;
        double       jump = 0; // pulo: para dentro da tela (y menor no espaço da dock)
        if (auto b = g_bouncing.find(g_items[i].cls); b != g_bouncing.end()) {
            const double t = elapsedMs(b->second);
            if (b->second.landAt < 0 || t < b->second.landAt)
                jump = dock::bounceOffset(t, cs);
        }
        render::drawTexture(render::iconTexture(iconName(g_items[i]), texPx), scr(c.x + off, c.y + off - jump, icon, icon));

        // bolinha de app aberto, no vão entre o ícone e a borda da tela
        if (!g_items[i].windows.empty()) {
            const double r = std::max(1.5, DOT_R * g.k);
            render::fillRect(scr(c.x + c.size / 2 - r, c.y + c.size + (dock::BARPAD * g.k) / 2 - r, 2 * r, 2 * r), CHyprColor{1, 1, 1, 0.85}, (int)std::ceil(px(r)));
        }
    }
    if (detached) {
        // ícone solto no cursor, com o aviso de que soltar ali remove da dock
        const double ix = g_drag.local.x - g.icon / 2, iy = g_drag.local.y - g.icon / 2;
        render::drawTexture(render::iconTexture(g_items[g_drag.from].cls, texPx), CBox{px(ix), px(iy), px(g.icon), px(g.icon)}, DETACHED_ALPHA);
        const double tw = 96, tx = g_drag.local.x - tw / 2, ty = iy - TIP_GAP - TIP_H;
        render::glassPanel(CBox{px(tx), px(ty), px(tw), px(TIP_H)}, (int)px(TIP_H / 2), sc, 0.4);
        const int lw = (int)std::lround(px(tw - 2 * TIP_PADX)), lh = (int)std::lround(px(TIP_H * 0.8));
        render::drawTexture(render::labelTexture("Remover", lw, lh), CBox{px(tx + TIP_PADX), px(ty + TIP_H * 0.2), (double)lw, (double)lh});
    } else if (dragging) {
        const auto&  first = l.cells.front();
        const auto&  last  = l.cells.back();
        const double x     = std::clamp(g_drag.x - cs / 2, first.x, last.x); // segue o cursor, sem sair da fileira
        const double off   = dock::PAD * g.k;
        render::drawTexture(render::iconTexture(g_items[g_drag.from].cls, texPx), scr(x + off, first.y + off - DRAG_LIFT, g.icon, g.icon));
    }

    const auto panelScr = dock::toScreen(f, {l.panel.x, l.panel.y, l.panel.w, l.panel.h});

    // menu de contexto, no monitor onde foi aberto
    if (g_menu.open && g_menu.mon.lock() == mon) {
        int         idx = -1;
        const auto* it  = g_menu.dock ? nullptr : menuItem(&idx);
        if (g_menu.dock || it) {
            // âncora: o item do app, ou o ponto do clique na fileira (menu da dock)
            const auto& c0 = l.cells.front();
            const auto  anchor = it ? dock::Box{l.cells[idx].x, l.cells[idx].y, l.cells[idx].size, l.cells[idx].size} :
                                      dock::Box{g_menu.at - c0.size / 2, c0.y, c0.size, c0.size};
            const auto  seps = menuSeps();
            const auto  m    = dock::menuGeometry(f, dock::toScreen(f, anchor), panelScr, seps);
            g_menu.g         = m;
            g_menu.drawn     = true;
            render::glassPanel(CBox{px(m.x), px(m.y), px(m.w), px(m.h)}, (int)px(MENU_RADIUS), sc, 0.5);
            const bool   checks = std::any_of(g_menu.rows.begin(), g_menu.rows.end(), [](Action a) { return checked(a) || a == Action::Magnify; });
            const double textX  = ROW_TEXT_X + (checks ? CHECK_W : 0);
            for (size_t r = 0; r < g_menu.rows.size(); r++) {
                const double ry = dock::menuRowY(m, seps, r);
                if (seps[r]) {
                    render::fillRect(CBox{px(m.x + ROW_TEXT_X), std::round(px(ry + dock::MENU_SEP / 2)), px(m.w - 2 * ROW_TEXT_X), std::max(1.0, std::round(sc))},
                                     CHyprColor{1, 1, 1, 0.14}, 0);
                    continue;
                }
                const int lh = (int)std::lround(px(dock::MENU_ROW * 0.72));
                if (g_menu.rows[r] == Action::MagLevel) {
                    // trilho + parte preenchida + botão + valor; apagado com a ampliação desligada
                    const float  a  = g_cfg.mag > 1 ? 1.F : 0.35F;
                    double       x0 = 0, x1 = 0;
                    sliderTrack(x0, x1);
                    const double kx = dock::sliderX(g_cfg.magLevel, x0, x1, dock::MAG_MIN, dock::MAG_MAX);
                    const double cy = ry + dock::MENU_ROW / 2;
                    render::fillRect(CBox{px(x0), px(cy - SLIDER_TRACK_H / 2), px(x1 - x0), px(SLIDER_TRACK_H)}, CHyprColor{1, 1, 1, 0.18 * a}, (int)px(SLIDER_TRACK_H / 2));
                    render::fillRect(CBox{px(x0), px(cy - SLIDER_TRACK_H / 2), px(kx - x0), px(SLIDER_TRACK_H)}, CHyprColor{0.04, 0.52, 1, 0.95 * a}, (int)px(SLIDER_TRACK_H / 2));
                    render::fillRect(CBox{px(kx - SLIDER_KNOB / 2), px(cy - SLIDER_KNOB / 2), px(SLIDER_KNOB), px(SLIDER_KNOB)}, CHyprColor{1, 1, 1, 0.95 * a}, (int)px(SLIDER_KNOB / 2));
                    const int vw = (int)std::lround(px(SLIDER_VALUE_W));
                    auto      v  = std::format("{:.1f}×", g_cfg.magLevel);
                    std::replace(v.begin(), v.end(), '.', ',');
                    render::drawTexture(render::labelTexture(v, vw, lh, false), CBox{px(m.x + m.w - ROW_TEXT_X - SLIDER_VALUE_W + 6), px(ry + dock::MENU_ROW * 0.16), (double)vw, (double)lh}, a);
                    continue;
                }
                if ((int)r == g_menu.hover)
                    render::fillRect(CBox{px(m.x + 4), px(ry), px(m.w - 8), px(dock::MENU_ROW)}, CHyprColor{1, 1, 1, 0.16}, (int)px(ROW_RADIUS));
                if (checked(g_menu.rows[r])) {
                    const int cw = (int)std::lround(px(CHECK_W));
                    render::drawTexture(render::labelTexture("✓", cw, lh, false), CBox{px(m.x + ROW_TEXT_X), px(ry + dock::MENU_ROW * 0.16), (double)cw, (double)lh});
                }
                const int lw = (int)std::lround(px(m.w - textX - ROW_TEXT_X));
                render::drawTexture(render::labelTexture(label(g_menu.rows[r], it), lw, lh, false),
                                    CBox{px(m.x + textX), px(ry + dock::MENU_ROW * 0.16), (double)lw, (double)lh});
                if (g_menu.rows[r] == Action::Position) {
                    const int aw = (int)std::lround(px(ARROW_W));
                    render::drawTexture(render::labelTexture("›", aw, lh, false), CBox{px(m.x + m.w - ROW_TEXT_X - ARROW_W + 4), px(ry + dock::MENU_ROW * 0.16), (double)aw, (double)lh});
                }
            }

            // submenu (Posição na Tela), ao lado da linha que o abriu
            if (g_menu.subRow >= 0 && (size_t)g_menu.subRow < g_menu.rows.size()) {
                const auto sm = dock::submenuGeometry(m, dock::menuRowY(m, seps, g_menu.subRow), g_menu.subRows.size(), mon->m_size.x, mon->m_size.y);
                g_menu.sg     = sm;
                render::glassPanel(CBox{px(sm.x), px(sm.y), px(sm.w), px(sm.h)}, (int)px(MENU_RADIUS), sc, 0.5);
                for (size_t r = 0; r < g_menu.subRows.size(); r++) {
                    const double ry = sm.y + dock::MENU_PAD + (double)r * dock::MENU_ROW;
                    if ((int)r == g_menu.subHover)
                        render::fillRect(CBox{px(sm.x + 4), px(ry), px(sm.w - 8), px(dock::MENU_ROW)}, CHyprColor{1, 1, 1, 0.16}, (int)px(ROW_RADIUS));
                    const int lh = (int)std::lround(px(dock::MENU_ROW * 0.72));
                    if (checked(g_menu.subRows[r])) {
                        const int cw = (int)std::lround(px(CHECK_W));
                        render::drawTexture(render::labelTexture("✓", cw, lh, false), CBox{px(sm.x + ROW_TEXT_X), px(ry + dock::MENU_ROW * 0.16), (double)cw, (double)lh});
                    }
                    const int lw = (int)std::lround(px(sm.w - 2 * ROW_TEXT_X - CHECK_W));
                    render::drawTexture(render::labelTexture(label(g_menu.subRows[r], nullptr), lw, lh, false),
                                        CBox{px(sm.x + ROW_TEXT_X + CHECK_W), px(ry + dock::MENU_ROW * 0.16), (double)lw, (double)lh});
                }
            }
        }
    }

    // tooltip com o nome do app ao lado do ícone sob o mouse, para dentro da tela (escondido com o menu aberto)
    if (hoverHere && !g_menu.open && !dragging && (size_t)g_hover < g_items.size()) {
        const auto&  c    = l.cells[g_hover];
        const auto   name = displayName(g_items[g_hover]);
        const double tw   = std::clamp(9.0 * (double)name.size() + 2 * TIP_PADX, 60.0, 360.0);
        const auto   t    = dock::besideItem(f, dock::toScreen(f, {c.x, c.y, c.size, c.size}), panelScr, tw, TIP_H, TIP_GAP);
        render::glassPanel(CBox{px(t.x), px(t.y), px(t.w), px(t.h)}, (int)px(TIP_H / 2), sc, 0.4);
        const int lw = (int)std::lround(px(tw - 2 * TIP_PADX)), lh = (int)std::lround(px(TIP_H * 0.8));
        render::drawTexture(render::labelTexture(name, lw, lh), CBox{px(t.x + TIP_PADX), px(t.y + TIP_H * 0.2), (double)lw, (double)lh});
    }

    // puffs: o ícone encolhe e some, a fumaça se espalha
    for (auto& p : g_poofs) {
        if (p.mon.lock() != mon)
            continue;
        const double t = msSince(p.start) / dock::POOF_MS;
        if (const double s = dock::poofIconScale(t) * g.icon; s > 0)
            render::drawTexture(render::iconTexture(p.cls, texPx), CBox{px(p.at.x - s / 2), px(p.at.y - s / 2), px(s), px(s)},
                                (float)(DETACHED_ALPHA * dock::poofIconAlpha(t)));
        for (auto& f : dock::poofPuffs(t, g.icon))
            render::drawTexture(smokeTexture(f.variant), CBox{px(p.at.x + f.dx - f.size / 2), px(p.at.y + f.dy - f.size / 2), px(f.size), px(f.size)}, (float)f.alpha);
    }
}

// ---------- redesenho ----------

// só a faixa do monitor junto da borda da dock: dock, ícones ampliados, tooltip, menu e sombras
void damageDock(const PHLMONITOR& mon) {
    for (auto& p : g_placed)
        if (p.mon.lock() == mon) {
            const double cs = p.g.icon + 2 * dock::PAD * p.g.k;
            // além dos ícones ampliados: o que sai da dock para dentro da tela. Embaixo, a etiqueta e o
            // menu sobem pouco; nas laterais eles saem para o lado e são largos (etiqueta até 360 px,
            // menu + submenu ~410 px). O que ficar fora daqui não é repintado e sobra na tela.
            const double reach = p.f.edge == dock::Edge::Bottom ? 260 : 2 * dock::MENU_W + dock::MENU_GAP + 40;
            const double top   = std::max(0.0, p.g.y - (magnification() - 1) * cs - reach);
            const auto   band = dock::toScreen(p.f, {0, top, p.f.w(), p.f.h() - top});
            g_pHyprRenderer->damageBox(CBox{mon->m_position.x + band.x, mon->m_position.y + band.y, band.w, band.h});
            return;
        }
    g_pHyprRenderer->damageMonitor(mon);
}

// ---------- reserva de espaço ----------
// O Hyprland zera e recalcula a parte "LS" da área reservada de cada monitor em todo rearranjo de
// layers (é por onde as barras reservam espaço). Um hook nesse rearranjo soma a faixa da dock logo
// depois e recalcula o layout das janelas; quando a faixa muda, pedimos um rearranjo.

// espessura reservada na borda: margem + painel (0 se não reserva / não há dock)
double reserveFor(const PHLMONITOR& mon) {
    if (!g_cfg.reserve || g_items.empty())
        return 0;
    const auto f = frameFor(mon);
    return std::ceil(f.h() - geometry(f).y);
}

#ifdef HYPRTAB_RESERVE
CFunctionHook*                              g_arrangeHook = nullptr;
std::unordered_map<MONITORID, std::string> g_reserved; // o que está aplicado em cada monitor (borda + px)
typedef void (*ArrangeFn)(void*, const MONITORID&);

void hkArrange(void* thisptr, const MONITORID& id) {
    ((ArrangeFn)g_arrangeHook->m_original)(thisptr, id);
    for (auto& mon : g_pCompositor->m_monitors) {
        if (mon->m_id != id)
            continue;
        const double r    = reserveFor(mon);
        const auto   edge = frameFor(mon).edge;
        g_reserved[id]    = std::format("{}:{}", (int)edge, r);
        if (r <= 0)
            return;
        const Vector2D tl = edge == dock::Edge::Left ? Vector2D{r, 0.0} : Vector2D{0.0, 0.0};
        const Vector2D br = edge == dock::Edge::Right ? Vector2D{r, 0.0} : edge == dock::Edge::Bottom ? Vector2D{0.0, r} : Vector2D{0.0, 0.0};
        mon->m_reservedArea.addType(Desktop::RESERVED_DYNAMIC_TYPE_LS, tl, br);
        g_pLayoutManager->getCurrentLayout()->recalculateMonitor(id);
        return;
    }
}

void installReserve() {
    for (auto& m : HyprlandAPI::findFunctionsByName(PHANDLE, "arrangeLayersForMonitor"))
        if (m.demangled.contains("CHyprRenderer::arrangeLayersForMonitor")) {
            g_arrangeHook = HyprlandAPI::createFunctionHook(PHANDLE, m.address, (void*)&hkArrange);
            if (g_arrangeHook && !g_arrangeHook->hook())
                g_arrangeHook = nullptr;
            break;
        }
}

void rearrangeAll() {
    for (auto& mon : g_pCompositor->m_monitors)
        g_pHyprRenderer->arrangeLayersForMonitor(mon->m_id);
}

// reaplica onde a faixa mudou (borda, tamanho, dock apareceu/sumiu, opção desligada)
void syncReserve() {
    if (!g_arrangeHook)
        return;
    for (auto& mon : g_pCompositor->m_monitors)
        if (g_reserved[mon->m_id] != std::format("{}:{}", (int)frameFor(mon).edge, reserveFor(mon)))
            g_pHyprRenderer->arrangeLayersForMonitor(mon->m_id);
}

void removeReserve() {
    if (!g_arrangeHook)
        return;
    HyprlandAPI::removeFunctionHook(PHANDLE, g_arrangeHook);
    g_arrangeHook = nullptr;
    rearrangeAll(); // sem o hook, o rearranjo devolve o espaço
}
#else
void installReserve() {}
void syncReserve() {}
void removeReserve() {}
#endif

// ---------- animação do zoom ----------

// um timer para as duas animações: zoom (entrada/saída) e pulo dos apps abrindo
bool applyResize();

void onAnim(SP<CEventLoopTimer> self, void*) {
    applyResize();
    const double step = ANIM_FRAME.count() / ANIM_MS;
    g_level           = g_levelTo > g_level ? std::min(g_levelTo, g_level + step) : std::max(g_levelTo, g_level - step);
    if (auto m = g_magMon.lock())
        damageDock(m);

    // enquanto há pulo (e no quadro em que o último pousa) a dock é redesenhada em todo monitor
    const bool landed = std::erase_if(g_bouncing, [](const auto& kv) { return kv.second.landAt >= 0 && elapsedMs(kv.second) >= kv.second.landAt; }) > 0;
    if (!g_bouncing.empty() || landed) {
        for (auto& p : g_placed)
            if (auto m = p.mon.lock())
                damageDock(m);
    }

    // puffs: redesenha só em volta de cada um; o que terminou recebe um último redesenho para sumir
    for (auto& p : g_poofs)
        if (auto m = p.mon.lock()) {
            const double r = g_cfg.iconSize * 3.5; // alcance das partículas (até ~2 ícones) + o tamanho delas
            g_pHyprRenderer->damageBox(CBox{m->m_position.x + p.at.x - r, m->m_position.y + p.at.y - r, 2 * r, 2 * r});
        }
    std::erase_if(g_poofs, [](const Poof& p) { return msSince(p.start) >= dock::POOF_MS; });

    if (g_entering) {
        damageAll(); // nos primeiros quadros a dock ainda não foi posicionada: redesenha o monitor
        if (!g_enterPending && msSince(g_enterStart) >= dock::ENTER_MS)
            g_entering = false;
    }

    if (g_level != g_levelTo || !g_bouncing.empty() || !g_poofs.empty() || g_entering || (g_resize.active && g_resize.want != g_cfg.iconSize))
        self->updateTimeout(ANIM_FRAME);
}

void startEnter() {
    g_entering     = true;
    g_enterPending = true;
    kickAnim();
}

void kickAnim() {
    if (g_anim && !g_anim->armed())
        g_anim->updateTimeout(ANIM_FRAME);
}

void animateTo(double target) {
    g_levelTo = target;
    if (g_level != g_levelTo)
        kickAnim();
}

// ---------- mouse ----------

// ponto global -> posição ao longo da fileira, no espaço da dock do monitor
double dockX(const PHLMONITOR& mon, const Vector2D& pos) {
    for (auto& p : g_placed)
        if (p.mon.lock() == mon) {
            double dx = 0, dy = 0;
            dock::toDock(p.f, pos.x - mon->m_position.x, pos.y - mon->m_position.y, dx, dy);
            return dx;
        }
    return 0;
}

// item sob o ponto global, em qualquer monitor; -2 se fora de toda dock
int hitGlobal(const Vector2D& pos, PHLMONITOR* monOut = nullptr) {
    for (auto& p : g_placed) {
        auto mon = p.mon.lock();
        if (!mon)
            continue;
        double dx = 0, dy = 0;
        dock::toDock(p.f, pos.x - mon->m_position.x, pos.y - mon->m_position.y, dx, dy);
        dy          = dock::snapToEdge(p.g, p.f.h(), dy); // encostado na borda da tela conta
        const int i = dock::hit(layoutFor(mon, p.g), dx, dy);
        if (i != -2) {
            if (monOut)
                *monOut = mon;
            return i;
        }
    }
    return -2;
}

// linha do menu sob o ponto global (-2 fora do menu)
// linha do submenu sob o ponto global (-2 fora dele ou fechado)
int subHitGlobal(const Vector2D& pos) {
    auto mon = g_menu.mon.lock();
    if (!g_menu.open || g_menu.subRow < 0 || !mon)
        return -2;
    return dock::menuHit(g_menu.sg, g_menu.subRows.size(), pos.x - mon->m_position.x, pos.y - mon->m_position.y);
}

// abre o submenu da linha `r` do menu (se ela tiver um) ou fecha o que estiver aberto
void setSubmenu(int r) {
    const bool has = r >= 0 && (size_t)r < g_menu.rows.size() && g_menu.rows[r] == Action::Position;
    const int  to  = has ? r : -1;
    if (to == g_menu.subRow)
        return;
    g_menu.subRow   = to;
    g_menu.subRows  = has ? std::vector<Action>{Action::PosLeft, Action::PosBottom, Action::PosRight} : std::vector<Action>{};
    g_menu.subHover = -1;
    damageAll();
}

int menuHitGlobal(const Vector2D& pos) {
    auto mon = g_menu.mon.lock();
    if (!g_menu.open || !g_menu.drawn || !mon)
        return -2;
    return dock::menuHit(g_menu.g, menuSeps(), pos.x - mon->m_position.x, pos.y - mon->m_position.y);
}

// zona do zoom: a extensão da dock (ampliada) e da base dela até a ponta dos ícones ampliados
bool updateZoom(const Vector2D& pos) {
    if (magnification() <= 1)
        return false;
    for (auto& p : g_placed) {
        auto mon = p.mon.lock();
        if (!mon)
            continue;
        double lx = 0, ly = 0; // no espaço da dock
        dock::toDock(p.f, pos.x - mon->m_position.x, pos.y - mon->m_position.y, lx, ly);
        const auto l = layoutFor(mon, p.g);
        const double cs = p.g.icon + 2 * dock::PAD * p.g.k;
        const double top = p.g.y - (magnification() - 1) * cs;
        // até a borda da tela: o cursor encostado no canto também amplia
        if (lx >= l.panel.x && lx <= l.panel.x + l.panel.w && ly >= top && ly <= p.f.h() + 1) {
            if (g_magMon.lock() != mon)
                g_level = 0; // entrou em outro monitor: começa do zero lá
            g_magMon = mon;
            g_magX   = lx;
            damageDock(mon);
            return true;
        }
    }
    return false;
}

// ---------- redimensionar (arrastar o separador ou uma área vazia, como no macOS) ----------
// Para dentro da tela aumenta, para a borda diminui: 1 px de mouse = 1 px de ícone. O tamanho muda ao
// vivo; a reserva de espaço e o arquivo de ajustes só no fim.



constexpr double RESIZE_START = 4; // px de movimento antes de começar (um clique parado não faz nada)

double dockY(const PHLMONITOR& mon, const Vector2D& pos) {
    for (auto& p : g_placed)
        if (p.mon.lock() == mon) {
            double dx = 0, dy = 0;
            dock::toDock(p.f, pos.x - mon->m_position.x, pos.y - mon->m_position.y, dx, dy);
            return dy;
        }
    return 0;
}

void updateResize(const Vector2D& pos) {
    auto mon = g_resize.mon.lock();
    if (!mon)
        return;
    if (!g_resize.active) {
        if (std::hypot(pos.x - g_resize.start.x, pos.y - g_resize.start.y) <= RESIZE_START)
            return;
        g_resize.active = true;
        g_level = g_levelTo = 0; // sem zoom enquanto redimensiona
    }
    // o mouse manda eventos bem mais rápido que os quadros: só guarda o pedido, o onAnim aplica
    g_resize.want = std::clamp(std::round(g_resize.startSize - (dockY(mon, pos) - g_resize.startDy)), ICON_MIN, ICON_MAX);
    kickAnim();
}

// um passo por quadro (onAnim): aplica o último tamanho pedido
bool applyResize() {
    if (!g_resize.active || g_resize.want == g_cfg.iconSize)
        return false;
    g_set.iconSize = g_resize.want;
    g_cfg.iconSize = g_resize.want;
    for (auto& p : g_placed)
        if (auto m = p.mon.lock())
            g_pHyprRenderer->damageMonitor(m); // a dock muda de tamanho: a faixa antiga também precisa sumir
    return true;
}

void updateCursor(const Vector2D& pos);

void endResize(bool commit) {
    const auto r = g_resize;
    g_resize     = {};
    updateCursor(g_mouse);
    if (!r.active)
        return;
    if (!commit) { // Esc: volta ao tamanho de antes
        g_set.iconSize = r.startSize;
        g_cfg.iconSize = r.startSize;
    } else if (r.want > 0) {
        g_set.iconSize = r.want;
        g_cfg.iconSize = r.want;
    }
    saveSettings();
    syncReserve();
    animateTo(updateZoom(g_mouse) ? 1 : 0);
    for (auto& p : g_placed)
        if (auto m = p.mon.lock())
            g_pHyprRenderer->damageMonitor(m);
}

// cursor de redimensionar onde o arraste muda o tamanho (separador / área vazia da dock)
#if __has_include(<hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>)
bool g_cursorSet = false;

void setResizeCursor(bool on) {
    if (on == g_cursorSet)
        return;
    g_cursorSet = on;
    if (on)
        Cursor::overrideController->setOverride(g_cfg.edge == dock::Edge::Bottom ? "ns-resize" : "ew-resize", Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    else
        Cursor::overrideController->unsetOverride(Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
}
#else
void setResizeCursor(bool) {}
#endif

// cursor em cima do separador (o vão entre fixos e abertos, da base da dock até a borda da tela)? Só
// ali o cursor vira o de redimensionar e o arraste muda o tamanho, como no macOS.
bool onSeparator(const Vector2D& pos) {
    for (auto& p : g_placed) {
        auto mon = p.mon.lock();
        if (!mon)
            continue;
        const auto l = layoutFor(mon, p.g);
        double     dx = 0, dy = 0;
        dock::toDock(p.f, pos.x - mon->m_position.x, pos.y - mon->m_position.y, dx, dy);
        const double half = (dock::SEP_W + dock::GAP) * p.g.k / 2;
        for (const double sx : l.sepX)
            if (std::abs(dx - sx) <= half && dy >= l.panel.y && dy <= p.f.h() + 1)
                return true;
    }
    return false;
}

void updateCursor(const Vector2D& pos) {
    setResizeCursor(g_resize.pressed || (!g_menu.open && !g_drag.pressed && onSeparator(pos)));
}

// ---------- arrastar ----------

size_t pinnedCount() {
    return (size_t)std::count_if(g_items.begin(), g_items.end(), [](const dock::Item& i) { return i.pinned; });
}

// cursor ao longo da fileira, no espaço da dock do monitor do arraste
void updateDrag(const Vector2D& pos) {
    auto mon = g_drag.mon.lock();
    for (auto& p : g_placed)
        if (mon && p.mon.lock() == mon) {
            double dx = 0, dy = 0;
            dock::toDock(p.f, pos.x - mon->m_position.x, pos.y - mon->m_position.y, dx, dy);
            const bool wasOutside = g_drag.outside;
            g_drag.x              = dx;
            g_drag.drop           = dock::dropIndex(p.g, g_items.size(), dx, 0, std::max<size_t>(1, pinnedCount()) - 1);
            g_drag.outside        = dock::dragOutside(p.g, dx, dy);
            g_drag.local          = {pos.x - mon->m_position.x, pos.y - mon->m_position.y};
            // solto, o ícone pode ir longe da faixa da dock (e deixar rastro ao voltar): redesenha o monitor
            if (g_drag.outside || wasOutside)
                g_pHyprRenderer->damageMonitor(mon);
            else
                damageDock(mon);
            return;
        }
}

void endDrag(bool commit) {
    const auto d = g_drag;
    g_drag       = {};
    if (commit && d.active && d.outside) { // soltou longe da dock: deixa de ser fixo, com um puff
        g_poofs.push_back({d.mon, d.local, d.cls, std::chrono::steady_clock::now()});
        kickAnim();
        dock::setPinned(g_pins, configPins(), d.cls, false);
        savePins();
        refresh();
    } else if (commit && d.active) {
        if (d.drop != d.from) {
            dock::reorderPin(g_pins, configPins(), d.cls, d.drop);
            savePins();
            refresh();
        }
    } else if (commit) { // não arrastou: é um clique
        const auto it = std::find_if(g_items.begin(), g_items.end(), [&](const dock::Item& i) { return i.cls == d.cls; });
        if (it != g_items.end())
            activate(*it);
    }
    animateTo(updateZoom(g_mouse) ? 1 : 0);
    if (d.outside) // o ícone solto estava fora da faixa da dock
        if (auto m = d.mon.lock())
            g_pHyprRenderer->damageMonitor(m);
    damageAll();
}

void onMouseMove(const Vector2D& pos) {
    g_mouse = pos;
    if (g_resize.pressed) {
        updateResize(pos);
        return;
    }
    if (g_slider) {
        sliderFromMouse(pos);
        return;
    }
    updateCursor(pos);
    if (g_drag.pressed) {
        // só os fixos arrastam; os outros esperam soltar e viram clique
        if (!g_drag.active && g_drag.from < g_items.size() && g_items[g_drag.from].pinned &&
            std::hypot(pos.x - g_drag.start.x, pos.y - g_drag.start.y) > DRAG_START) {
            g_drag.active = true;
            g_level = g_levelTo = 0;
        }
        if (g_drag.active) {
            updateDrag(pos);
            return;
        }
    }
    // menu aberto: o zoom congela (como no macOS), mesmo com o mouse indo até o menu
    if (!g_menu.open)
        animateTo(updateZoom(pos) ? 1 : 0);
    const int h  = std::max(-1, hitGlobal(pos));
    const int sr = subHitGlobal(pos);
    int       r  = std::max(-1, menuHitGlobal(pos));
    if (g_menu.open && sr != -2) {
        // no submenu: a linha que o abriu continua marcada
        r = g_menu.subRow;
        if (std::max(-1, sr) != g_menu.subHover) {
            g_menu.subHover = std::max(-1, sr);
            damageAll();
        }
    } else if (g_menu.open && r >= 0) {
        setSubmenu(r); // passou para outra linha: abre o submenu dela ou fecha o aberto
        g_menu.subHover = -1;
    }
    if (h != g_hover || (g_menu.open && r != g_menu.hover)) {
        g_hover      = h;
        g_menu.hover = r;
        damageAll();
    }
}

// retorna true se o clique deve ser consumido (foi na dock)
bool onMouseButton(const IPointer::SButtonEvent& ev) {
    // soltar o esquerdo depois de pressionar num item: termina o arraste ou é o clique
    if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED && ev.button == BTN_LEFT_ && g_drag.pressed) {
        g_swallowRelease = false;
        endDrag(true);
        return true;
    }
    if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED && ev.button == BTN_LEFT_ && g_slider) {
        g_slider         = false;
        g_swallowRelease = false;
        saveSettings();
        return true;
    }
    if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED && ev.button == BTN_LEFT_ && g_resize.pressed) {
        g_swallowRelease = false;
        endResize(true);
        return true;
    }
    if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        const bool s     = g_swallowRelease;
        g_swallowRelease = false;
        return s;
    }

    // com o menu aberto: clique esquerdo numa linha executa; botão direito fecha (no mesmo app,
    // no menu ou fora) ou, em outro app da dock, já abre o menu dele; clique esquerdo fora fecha
    if (g_menu.open) {
        if (const int sr = subHitGlobal(g_mouse); sr != -2) { // no submenu
            if (ev.button == BTN_LEFT_ && sr >= 0 && (size_t)sr < g_menu.subRows.size()) {
                const auto action = g_menu.subRows[sr];
                closeMenu();
                runDock(action);
            }
            g_swallowRelease = true;
            return true;
        }
        const int r = menuHitGlobal(g_mouse);
        if (ev.button == BTN_LEFT_ && r >= 0 && (size_t)r < g_menu.rows.size() && g_menu.rows[r] == Action::Position) {
            setSubmenu(r); // clique na linha do submenu: só abre
            g_swallowRelease = true;
            return true;
        }
        if (ev.button == BTN_LEFT_ && r >= 0 && (size_t)r < g_menu.rows.size() && g_menu.rows[r] == Action::MagLevel) {
            // controle: arrasta sem fechar o menu (desligada a ampliação, não faz nada)
            if (g_cfg.mag > 1) {
                g_slider = true;
                sliderFromMouse(g_mouse);
            }
            g_swallowRelease = true;
            return true;
        }
        if (ev.button == BTN_LEFT_ && r >= 0 && (size_t)r < g_menu.rows.size()) {
            const auto action = g_menu.rows[r];
            const bool isDock = g_menu.dock;
            closeMenu();
            if (isDock)
                runDock(action);
            else if (const auto* it = menuItem())
                run(action, *it);
        } else if (ev.button == BTN_RIGHT_ || r == -2) {
            const auto prev = g_menu.cls;
            closeMenu();
            PHLMONITOR mon;
            const int  i = r == -2 ? hitGlobal(g_mouse, &mon) : -1;
            if (ev.button == BTN_RIGHT_ && i >= 0 && (size_t)i < g_items.size() && g_items[i].cls != prev)
                openMenu(i, mon);
        }
        g_swallowRelease = true;
        return true;
    }

    PHLMONITOR mon;
    const int  i = hitGlobal(g_mouse, &mon);
    if (i == -2)
        return false;
    g_swallowRelease = true;
    if (i >= 0 && (size_t)i < g_items.size()) {
        if (ev.button == BTN_LEFT_) // a ação fica para quando soltar (pode virar arraste)
            g_drag = {true, false, g_items[i].cls, (size_t)i, (size_t)i, mon, g_mouse, 0};
        else if (ev.button == BTN_RIGHT_)
            openMenu(i, mon);
    } else if (i == -1 && ev.button == BTN_RIGHT_) // separador ou área vazia da dock: menu da dock
        openDockMenu(mon, dockX(mon, g_mouse));
    else if (i == -1 && ev.button == BTN_LEFT_ && onSeparator(g_mouse)) { // separador: arrastar muda o tamanho
        g_resize = {true, false, mon, g_mouse, dockY(mon, g_mouse), g_cfg.iconSize, g_cfg.iconSize, 0, 0};
        for (auto& p : g_placed)
            if (p.mon.lock() == mon) {
                g_resize.startIcon  = p.g.icon;
                g_resize.startPanel = p.g.h;
            }
    }
    return true;
}

// ---------- teclado ----------

// retorna true se a tecla deve ser consumida: Esc fecha o menu ou cancela o arraste
bool onKey(const IKeyboard::SKeyEvent& ev) {
    if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED || ev.keycode != KEY_ESC_)
        return false;
    if (g_drag.active) { // Esc no meio do arraste: cancela, a ordem não muda
        endDrag(false);
        return true;
    }
    if (g_resize.active) { // Esc redimensionando: volta ao tamanho de antes
        endResize(false);
        return true;
    }
    if (!g_menu.open)
        return false;
    closeMenu();
    return true;
}

#ifdef HYPRTAB_EVENT_BUS
CHyprSignalListener g_moveListener, g_buttonListener, g_keyListener;

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
}
void unregisterEvents() {
    g_moveListener.reset();
    g_buttonListener.reset();
    g_keyListener.reset();
}
#else
SP<HOOK_CALLBACK_FN> g_moveCb, g_buttonCb, g_keyCb;

void registerEvents() {
    g_keyCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "keyPress", [](void*, SCallbackInfo& info, std::any data) {
        try {
            auto map = std::any_cast<std::unordered_map<std::string, std::any>>(data);
            if (onKey(std::any_cast<IKeyboard::SKeyEvent>(map["event"])))
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
    g_moveCb.reset();
    g_buttonCb.reset();
    g_keyCb.reset();
}
#endif

// `hyprdock:menu`: abre (ou fecha) o menu da dock no monitor do mouse, no meio da fileira
SDispatchResult dispMenu(std::string) {
    if (g_menu.open) {
        closeMenu();
        return {};
    }
    const auto mon = render::currentMonitor();
    for (auto& p : g_placed)
        if (mon && p.mon.lock() == mon) {
            openDockMenu(mon, p.g.x + p.g.w / 2);
            break;
        }
    return {};
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    addConfig();
    loadSettings();
    loadConfig();
    loadPins();
    registerEvents();
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprdock:menu", dispMenu);
    render::registerRender(draw);

    g_poll = makeShared<CEventLoopTimer>(std::nullopt, onPoll, nullptr);
    g_pEventLoopManager->addTimer(g_poll);
    refresh();
    g_poll->updateTimeout(POLL_EVERY);

    g_anim = makeShared<CEventLoopTimer>(std::nullopt, onAnim, nullptr);
    g_pEventLoopManager->addTimer(g_anim);

    installReserve();
    syncReserve();
    startEnter();

    return {"hyprdock", "Dock com zoom", "caio", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    for (auto* t : {&g_poll, &g_anim})
        if (*t) {
            g_pEventLoopManager->removeTimer(*t);
            t->reset();
        }
    unregisterEvents();
    setResizeCursor(false);
    render::unregisterRender();
    g_menu.open = false;
    g_items.clear();
    removeReserve();
    damageAll();
    g_placed.clear();
}
