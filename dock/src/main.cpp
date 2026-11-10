// hyprdock - dock estilo macOS para o Hyprland, tudo dentro do .so.
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

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

enum class Action { Open, Close, Maximize, Minimize, Pin, Unpin };

struct Menu {
    bool                open = false;
    std::string         cls; // por classe: os índices mudam quando a lista é relida
    PHLMONITORREF       mon;
    std::vector<Action> rows;
    int                 hover = -1;
    dock::MenuGeom      g     = {}; // local ao monitor, atualizado no draw
    bool                drawn = false;
} g_menu;

constexpr double MENU_RADIUS = 12, ROW_RADIUS = 8, ROW_TEXT_X = 14;

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
    bool                     showRunning = true, reserve = true;
} g_cfg;

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
    return g_cfg.edge != old;
}

// ---------- fixos persistidos ----------

std::filesystem::path pinsPath() {
    if (const char* x = getenv("XDG_STATE_HOME"); x && *x)
        return std::filesystem::path(x) / "hyprdock/pins";
    if (const char* h = getenv("HOME"))
        return std::filesystem::path(h) / ".local/state/hyprdock/pins";
    return {};
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

void activate(const dock::Item& it) {
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
    if (g_menu.open && std::none_of(g_items.begin(), g_items.end(), [](const dock::Item& i) { return i.cls == g_menu.cls; }))
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
    std::vector<Action> rows;
    if (it.windows.empty())
        rows.push_back(Action::Open);
    else
        rows.insert(rows.end(), {Action::Close, Action::Maximize, Action::Minimize});
    rows.push_back(it.pinned ? Action::Unpin : Action::Pin);
    return rows;
}

std::string label(Action a, const dock::Item& it) {
    switch (a) {
        case Action::Open: return "Abrir";
        case Action::Close: return it.windows.size() > 1 ? std::format("Fechar todas ({})", it.windows.size()) : "Fechar";
        case Action::Maximize: return "Maximizar";
        case Action::Minimize: return "Minimizar";
        case Action::Pin: return "Fixar na Dock";
        case Action::Unpin: return "Remover da Dock";
    }
    return "";
}

void run(Action a, const dock::Item& it) {
    const auto target = dock::targetWindow(it, focusedAddr());
    switch (a) {
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
    g_menu  = {true, g_items[i].cls, mon, menuRows(g_items[i]), -1, {}, false};
    damageAll(); // o zoom fica congelado como está (ver onMouseMove); o menu ancora no item ampliado
}

void closeMenu() {
    if (!g_menu.open)
        return;
    g_menu.open = false;
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

dock::Geom geometry(const dock::Frame& f, size_t n) {
    return dock::geometry(n, f.w(), f.h(), g_cfg.iconSize, g_cfg.margin);
}
dock::Geom geometry(const dock::Frame& f) {
    return geometry(f, g_items.size());
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
    if (g_items.empty() || hyprtabHasFullscreen(mon))
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
    const int texPx = std::max(16, (int)std::lround(px(g.icon * mag)));
    // entrando: o primeiro quadro carrega os ícones (disco, pode levar centenas de ms) antes de o relógio
    // começar; senão a animação já estaria no fim quando a dock aparecesse
    if (g_enterPending) {
        for (auto& it : g_items)
            render::iconTexture(it.cls, texPx);
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
        l = dock::magnify(geometry(f, n - 1), n - 1, 0, 1, 0);
    auto slot = [&](size_t i) { return !dragging ? i : detached ? (i > g_drag.from ? i - 1 : i) : dock::dragSlot(i, g_drag.from, g_drag.drop); };

    if (!(detached && n == 1)) // o único item saiu: não sobra dock
        render::glassPanel(scr(l.panel.x, l.panel.y, l.panel.w, l.panel.h), (int)px(RADIUS * g.k), sc, g.k);

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
        render::drawTexture(render::iconTexture(g_items[i].cls, texPx), scr(c.x + off, c.y + off - jump, icon, icon));

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
        int idx = -1;
        if (const auto* it = menuItem(&idx)) {
            const auto& c = l.cells[idx];
            const auto  m = dock::menuGeometry(f, dock::toScreen(f, {c.x, c.y, c.size, c.size}), panelScr, g_menu.rows.size());
            g_menu.g      = m;
            g_menu.drawn  = true;
            render::glassPanel(CBox{px(m.x), px(m.y), px(m.w), px(m.h)}, (int)px(MENU_RADIUS), sc, 0.5);
            for (size_t r = 0; r < g_menu.rows.size(); r++) {
                const double ry = m.y + dock::MENU_PAD + (double)r * dock::MENU_ROW;
                if ((int)r == g_menu.hover)
                    render::fillRect(CBox{px(m.x + 4), px(ry), px(m.w - 8), px(dock::MENU_ROW)}, CHyprColor{1, 1, 1, 0.16}, (int)px(ROW_RADIUS));
                const int lw = (int)std::lround(px(m.w - 2 * ROW_TEXT_X)), lh = (int)std::lround(px(dock::MENU_ROW * 0.72));
                render::drawTexture(render::labelTexture(label(g_menu.rows[r], *it), lw, lh, false),
                                    CBox{px(m.x + ROW_TEXT_X), px(ry + dock::MENU_ROW * 0.16), (double)lw, (double)lh});
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
            const double cs   = p.g.icon + 2 * dock::PAD * p.g.k;
            const double top  = std::max(0.0, p.g.y - (magnification() - 1) * cs - 260);
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
void onAnim(SP<CEventLoopTimer> self, void*) {
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

    if (g_level != g_levelTo || !g_bouncing.empty() || !g_poofs.empty() || g_entering)
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

// item sob o ponto global, em qualquer monitor; -2 se fora de toda dock
int hitGlobal(const Vector2D& pos, PHLMONITOR* monOut = nullptr) {
    for (auto& p : g_placed) {
        auto mon = p.mon.lock();
        if (!mon)
            continue;
        double dx = 0, dy = 0;
        dock::toDock(p.f, pos.x - mon->m_position.x, pos.y - mon->m_position.y, dx, dy);
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
int menuHitGlobal(const Vector2D& pos) {
    auto mon = g_menu.mon.lock();
    if (!g_menu.open || !g_menu.drawn || !mon)
        return -2;
    return dock::menuHit(g_menu.g, g_menu.rows.size(), pos.x - mon->m_position.x, pos.y - mon->m_position.y);
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
        if (lx >= l.panel.x && lx <= l.panel.x + l.panel.w && ly >= top && ly <= p.g.y + p.g.h) {
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
    const int h = std::max(-1, hitGlobal(pos));
    const int r = std::max(-1, menuHitGlobal(pos));
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
    if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        const bool s     = g_swallowRelease;
        g_swallowRelease = false;
        return s;
    }

    // com o menu aberto: clique esquerdo numa linha executa; botão direito fecha (no mesmo app,
    // no menu ou fora) ou, em outro app da dock, já abre o menu dele; clique esquerdo fora fecha
    if (g_menu.open) {
        const int r = menuHitGlobal(g_mouse);
        if (ev.button == BTN_LEFT_ && r >= 0 && (size_t)r < g_menu.rows.size()) {
            const auto action = g_menu.rows[r];
            closeMenu();
            if (const auto* it = menuItem())
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

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    addConfig();
    loadConfig();
    loadPins();
    registerEvents();
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

    return {"hyprdock", "Dock estilo macOS", "caio", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    for (auto* t : {&g_poll, &g_anim})
        if (*t) {
            g_pEventLoopManager->removeTimer(*t);
            t->reset();
        }
    unregisterEvents();
    render::unregisterRender();
    g_menu.open = false;
    g_items.clear();
    removeReserve();
    damageAll();
    g_placed.clear();
}
