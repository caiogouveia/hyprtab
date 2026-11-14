// hyprpager - pager de workspaces no estilo dos temas metálicos clássicos do Enlightenment, tudo dentro
// do .so (C++). Uma coluna de telas de TV antigas, uma por workspace, ligadas por canos de latão; dentro
// de cada tela, as janelas daquele workspace (retângulo + ícone do app) na posição e proporção reais.
//
// Clique numa tela: vai para o workspace. Clique numa janela dentro dela: vai e foca a janela.
// Arrastar uma janela para outra tela: muda a janela de workspace (sem sair do atual). Esc cancela.
//
// Config (hyprlang, até 0.56):
//   plugin {
//       hyprpager {
//           workspaces = 4        # quantas telas (workspaces 1..N)
//           size = 64             # largura de cada tela, px
//           position = left       # left ou right
//           align = center        # top, center ou bottom
//           offset = 0            # deslocamento vertical, px (positivo desce)
//           margin = 12           # distância da borda, px
//           monitor =             # nomes separados por vírgula; vazio: todos
//       }
//   }
// Config Lua (0.57+): hl.config({ plugin = { hyprpager = { workspaces = 4, align = "top", ... } } })

#include "../../src/icons.hpp"
#include "../../src/render.hpp"
#include "chrome.hpp"
#include "model.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#if __has_include(<hyprland/src/config/values/types/IntValue.hpp>)
#include <optional> // IValue.hpp usa std::optional sem incluir
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#define HYPRPAGER_CFG_V2 1
#endif

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <vector>

static constexpr uint32_t BTN_LEFT_ = 0x110, BTN_RIGHT_ = 0x111;
static constexpr uint32_t KEY_ESC_  = 1;

// janelas se mexem (animações, arrastar): o pager confere nesse intervalo e redesenha se algo mudou
static constexpr auto POLL_EVERY = std::chrono::milliseconds(100);

namespace {

constexpr double DRAG_START = 5; // px de movimento antes de virar arraste
const CHyprColor WIN_FILL{0.55, 0.78, 0.62, 0.22}, WIN_EDGE{0.70, 0.90, 0.75, 0.65}, WIN_FOCUS{0.85, 1.0, 0.88, 0.9};

// ---------- config ----------

// Valores e padrões. 0.55+: addConfigValueV2 (serve para hyprlang e para Lua; na 0.57 a API antiga
// virou no-op). Antes: addConfigValue/getConfigValue do hyprlang.
struct IntOpt {
    const char*   name;
    Hyprlang::INT def;
};
struct StrOpt {
    const char* name;
    const char* def;
};
constexpr IntOpt OPT_WORKSPACES{"plugin:hyprpager:workspaces", 4}, OPT_SIZE{"plugin:hyprpager:size", 64}, OPT_MARGIN{"plugin:hyprpager:margin", 12},
    OPT_OFFSET{"plugin:hyprpager:offset", 0};
constexpr StrOpt OPT_POSITION{"plugin:hyprpager:position", "left"}, OPT_ALIGN{"plugin:hyprpager:align", "center"}, OPT_MONITOR{"plugin:hyprpager:monitor", ""};

#ifdef HYPRPAGER_CFG_V2

std::unordered_map<std::string, SP<Config::Values::CIntValue>>    g_ints;
std::unordered_map<std::string, SP<Config::Values::CStringValue>> g_strs;

void addConfig() {
    for (auto& o : {OPT_WORKSPACES, OPT_SIZE, OPT_MARGIN, OPT_OFFSET}) {
        auto v = makeShared<Config::Values::CIntValue>(o.name, "hyprpager", (Config::INTEGER)o.def);
        if (HyprlandAPI::addConfigValueV2(PHANDLE, v))
            g_ints[o.name] = v;
    }
    for (auto& o : {OPT_POSITION, OPT_ALIGN, OPT_MONITOR}) {
        auto v = makeShared<Config::Values::CStringValue>(o.name, "hyprpager", Config::STRING{o.def});
        if (HyprlandAPI::addConfigValueV2(PHANDLE, v))
            g_strs[o.name] = v;
    }
}

Hyprlang::INT cfgInt(const IntOpt& o) {
    auto it = g_ints.find(o.name);
    return it != g_ints.end() ? (Hyprlang::INT)it->second->value() : o.def;
}
std::string cfgStr(const StrOpt& o) {
    auto it = g_strs.find(o.name);
    if (it == g_strs.end())
        return o.def;
    auto s = it->second->value();
    return s == "[[EMPTY]]" ? "" : s;
}

#else

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

void addConfig() {
    for (auto& o : {OPT_WORKSPACES, OPT_SIZE, OPT_MARGIN, OPT_OFFSET})
        HyprlandAPI::addConfigValue(PHANDLE, o.name, Hyprlang::INT{o.def});
    for (auto& o : {OPT_POSITION, OPT_ALIGN, OPT_MONITOR})
        HyprlandAPI::addConfigValue(PHANDLE, o.name, Hyprlang::STRING{o.def});
}

Hyprlang::INT cfgInt(const IntOpt& o) {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, o.name);
    return v ? **(Hyprlang::INT* const*)v->getDataStaticPtr() : o.def;
}
std::string cfgStr(const StrOpt& o) {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, o.name);
    if (!v)
        return o.def;
    auto s = *(Hyprlang::STRING const*)v->getDataStaticPtr();
    return s ? s : o.def;
}

#pragma GCC diagnostic pop

#endif

struct Config {
    int              workspaces = 4;
    double           size       = 64;
    pager::Placement at;
    std::string      monitor; // vazio: todos

    auto key() const {
        return std::make_tuple(workspaces, size, at.edge, at.align, at.margin, at.offset, monitor);
    }
} g_cfg;

bool loadConfig() {
    const auto old   = g_cfg.key();
    g_cfg.workspaces = (int)std::clamp<Hyprlang::INT>(cfgInt(OPT_WORKSPACES), 1, 20);
    g_cfg.size       = (double)std::clamp<Hyprlang::INT>(cfgInt(OPT_SIZE), 24, 300);
    g_cfg.at.margin  = (double)cfgInt(OPT_MARGIN);
    g_cfg.at.offset  = (double)cfgInt(OPT_OFFSET);
    g_cfg.at.edge    = pager::parseEdge(cfgStr(OPT_POSITION));
    g_cfg.at.align   = pager::parseAlign(cfgStr(OPT_ALIGN));
    g_cfg.monitor    = cfgStr(OPT_MONITOR);
    return old != g_cfg.key();
}

// ---------- janelas ----------

struct Win {
    PHLWINDOWREF w;
    std::string  addr, cls;
    int          ws = 0;      // id do workspace
    pager::Box   box;         // no monitor dela (relativa ao canto)
    double       monW = 0, monH = 0;
    bool         focused = false;
};

// janelas dos workspaces 1..N, na ordem de empilhamento (as de cima por último)
std::vector<Win> windows() {
    std::vector<Win> out;
    const auto       focus = hyprtabFocusedWindow();
    for (auto& w : hyprtabWindows()) {
        if (!w || !hyprtabMapped(w) || w->isHidden() || !w->m_workspace || w->onSpecialWorkspace())
            continue;
        const int ws = (int)w->workspaceID();
        if (ws < 1 || ws > g_cfg.workspaces)
            continue;
        auto mon = w->m_monitor.lock();
        if (!mon)
            continue;
        const auto b = w->getWindowMainSurfaceBox();
        out.push_back({w, hyprtabAddr(w), w->m_class, ws, {b.x - mon->m_position.x, b.y - mon->m_position.y, b.w, b.h}, mon->m_size.x, mon->m_size.y,
                       w == focus});
    }
    return out;
}

int activeWorkspace(const PHLMONITOR& mon) {
    return mon && mon->m_activeWorkspace ? (int)mon->m_activeWorkspace->m_id : 0;
}

// ---------- estado ----------

struct Placed {
    PHLMONITORREF mon;
    pager::Layout l;
    pager::Box    at; // pager no monitor (px lógicos)
};
std::vector<Placed> g_placed;

Vector2D g_mouse;
int      g_hover = -1; // tela sob o mouse (no monitor do mouse)

struct Press {
    bool          pressed = false, dragging = false;
    PHLMONITORREF mon;
    int           screen = -1; // tela onde pressionou
    std::string   addr, cls;   // janela pressionada (vazio: a tela)
    Vector2D      start;
    int           drop = -1;   // tela sob o cursor arrastando
} g_press;
bool g_swallowRelease = false;

SP<CEventLoopTimer> g_poll;
std::string         g_sig; // assinatura do que está desenhado (workspaces, janelas, foco)

// ---------- texturas ----------

// camadas do desenho (ver chrome.hpp), com cache por tamanho e escala. O refletor tem LAMP_STEPS + 1
// variações (pontos da rotação), geradas na primeira vez que aparecem e só reusadas depois.
constexpr int LAMP_STEPS = 40;
enum class Layer { Base, Gloss, Pool, Lamp };

render::Tex layerTex(Layer which, const pager::Layout& l, double scale, int step = 0) {
    static std::unordered_map<std::string, render::Tex> cache;
    const auto key = std::format("{}:{}:{}:{}:{}", (int)which, l.screens.size(), l.screenW, scale, step);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    if (cache.size() > 400)
        cache.clear();
    cairo_surface_t* s = nullptr;
    switch (which) {
        case Layer::Base: s = pager::drawBase(l, scale); break;
        case Layer::Gloss: s = pager::drawGloss(l, scale); break;
        case Layer::Pool: s = pager::drawLightPool(l, scale); break;
        case Layer::Lamp: s = pager::drawLamp(l, scale, (double)step / LAMP_STEPS); break;
    }
    auto t = render::makeTexture(s);
    cairo_surface_destroy(s);
    return cache[key] = t;
}

// ---------- refletores ----------
// Em que ponto da rotação está o refletor de cada tela (0 = para cima, apagado; 1 = virado para a
// tela, aceso), por monitor: cada monitor tem o seu workspace atual. O timer leva cada um em direção ao
// alvo (1 só na tela do workspace atual). Ao carregar, começam apagados: o do atual acende.

constexpr double LAMP_MS = 420;
constexpr auto   ANIM_FRAME = std::chrono::milliseconds(16);

std::unordered_map<MONITORID, std::vector<double>> g_lamps;
SP<CEventLoopTimer>                                g_anim;

void kickAnim() {
    if (g_anim && !g_anim->armed())
        g_anim->updateTimeout(ANIM_FRAME);
}

// alvo de cada tela no monitor; true se algum refletor ainda não chegou
bool lampsMoving(const PHLMONITOR& mon) {
    auto it = g_lamps.find(mon->m_id);
    if (it == g_lamps.end())
        return false;
    const int ws = activeWorkspace(mon);
    for (size_t i = 0; i < it->second.size(); i++)
        if (it->second[i] != ((int)i + 1 == ws ? 1.0 : 0.0))
            return true;
    return false;
}

// área reservada do monitor (o membro mudou de forma entre versões)
template <class M>
pager::Reserved reserved(const M& mon) {
    if constexpr (requires { mon->m_reservedArea.left(); })
        return {mon->m_reservedArea.top(), mon->m_reservedArea.right(), mon->m_reservedArea.bottom(), mon->m_reservedArea.left()};
    else if constexpr (requires { mon->m_reservedTopLeft.x; })
        return {mon->m_reservedTopLeft.y, mon->m_reservedBottomRight.x, mon->m_reservedBottomRight.y, mon->m_reservedTopLeft.x};
    else
        return {};
}

// ---------- desenho ----------

void draw(const PHLMONITOR& mon) {
    std::erase_if(g_placed, [&](const Placed& p) { return p.mon.expired() || p.mon.lock() == mon; });
    if (hyprtabLocked() || hyprtabHasFullscreen(mon) || !pager::monitorMatches(g_cfg.monitor, mon->m_name))
        return;

    const double sc = mon->m_scale;
    const auto   l  = pager::layout((size_t)g_cfg.workspaces, g_cfg.size);
    const auto   at = pager::place(l, g_cfg.at, mon->m_size.x, mon->m_size.y, reserved(mon));
    g_placed.push_back({mon, l, at});
    auto px  = [&](double v) { return v * sc; };
    auto box = [&](const pager::Box& b) { return CBox{px(at.x + b.x), px(at.y + b.y), px(b.w), px(b.h)}; };

    const CBox whole{std::round(px(at.x)), std::round(px(at.y)), std::ceil(l.w * sc), std::ceil(l.h * sc)};
    auto&      lamps = g_lamps[mon->m_id];
    if (lamps.size() != l.screens.size())
        lamps.assign(l.screens.size(), 0.0);
    if (lampsMoving(mon))
        kickAnim();
    // célula de uma tela (refletor, luz) no monitor, em px físicos inteiros
    auto cellBox = [&](size_t i) {
        const auto c = pager::lampCell(l, i);
        return CBox{std::round(px(at.x + c.x)), std::round(px(at.y + c.y)), std::ceil(c.w * sc), std::ceil(c.h * sc)};
    };
    render::drawTexture(layerTex(Layer::Base, l, sc), whole);

    const bool mouseHere = mon->logicalBox().containsPoint(g_mouse);
    if (mouseHere && g_hover >= 0 && (size_t)g_hover < l.screens.size() && !g_press.dragging)
        render::fillRect(box(l.screens[g_hover]), CHyprColor{1, 1, 1, 0.06}, (int)px(l.screenW * pager::RADIUS));
    if (g_press.dragging && g_press.mon.lock() == mon && g_press.drop >= 0 && g_press.drop != g_press.screen)
        render::fillRect(box(l.screens[g_press.drop]), CHyprColor{0.55, 0.85, 0.65, 0.16}, (int)px(l.screenW * pager::RADIUS));

    // janelas: retângulo esverdeado (fósforo de tela antiga) com o ícone do app no meio
    const double edge = 1 / sc; // 1 px físico
    for (auto& w : windows()) {
        if (g_press.dragging && w.addr == g_press.addr)
            continue; // a arrastada vai com o cursor
        const auto area = pager::fitMonitor(l.screens[w.ws - 1], w.monW, w.monH);
        const auto t    = pager::thumb(area, w.monW, w.monH, w.box);
        if (t.w < 1 || t.h < 1)
            continue;
        render::fillRect(box(t), WIN_FILL, 0);
        const auto& c = w.focused ? WIN_FOCUS : WIN_EDGE;
        for (auto& e : {pager::Box{t.x, t.y, t.w, edge}, pager::Box{t.x, t.y + t.h - edge, t.w, edge}, pager::Box{t.x, t.y, edge, t.h},
                        pager::Box{t.x + t.w - edge, t.y, edge, t.h}})
            render::fillRect(box(e), c, 0);
        const double ic = std::min({t.w, t.h} ) * 0.7;
        if (ic >= 6) {
            const double s = std::min(ic, l.screenH * 0.36);
            render::drawTexture(render::iconTexture(w.cls, std::max(16, (int)std::ceil(px(s) / 8) * 8)),
                                box({t.x + (t.w - s) / 2, t.y + (t.h - s) / 2, s, s}));
        }
    }

    // luz no vidro das telas iluminadas, por cima das janelas e por baixo dos reflexos
    for (size_t i = 0; i < l.screens.size(); i++)
        if (const double lit = pager::lampLit(lamps[i]); lit > 0)
            render::drawTexture(layerTex(Layer::Pool, l, sc), cellBox(i), (float)lit);
    render::drawTexture(layerTex(Layer::Gloss, l, sc), whole);
    // refletores, no ponto da rotação de cada um
    for (size_t i = 0; i < l.screens.size(); i++)
        render::drawTexture(layerTex(Layer::Lamp, l, sc, (int)std::lround(lamps[i] * LAMP_STEPS)), cellBox(i));

    // arrastando: o ícone da janela segue o cursor
    if (g_press.dragging && mouseHere) {
        const double s = l.screenH * 0.45, lx = g_mouse.x - mon->m_position.x, ly = g_mouse.y - mon->m_position.y;
        render::drawTexture(render::iconTexture(g_press.cls, std::max(16, (int)std::ceil(px(s) / 8) * 8)), CBox{px(lx - s / 2), px(ly - s / 2), px(s), px(s)}, 0.85F);
    }
}

void damageAll() {
    for (auto& p : g_placed)
        if (auto m = p.mon.lock()) {
            const double pad = p.l.screenW * 0.5; // refletores, facho e sombras passam um pouco das bordas
            g_pHyprRenderer->damageBox(CBox{m->m_position.x + p.at.x - pad, m->m_position.y + p.at.y - pad, p.at.w + 2 * pad, p.at.h + 2 * pad});
        }
    if (g_placed.empty())
        for (auto& m : g_pCompositor->m_monitors)
            if (m)
                g_pHyprRenderer->damageMonitor(m);
}

// ---------- ações ----------

void switchTo(int ws) {
    hyprtabDispatch(std::format("workspace {}", ws), std::format("hl.dsp.focus({{ workspace = {} }})", ws));
}

void moveTo(const std::string& addr, int ws) {
    hyprtabDispatch(std::format("movetoworkspacesilent {},address:{}", ws, addr),
                    std::format("hl.dsp.window.move({{ workspace = {}, follow = false, window = 'address:{}' }})", ws, addr));
}

// ---------- mouse ----------

// pager do monitor sob o ponto global, e o ponto local a ele; nullptr se fora de todo pager
const Placed* pagerAt(const Vector2D& pos, double& lx, double& ly) {
    for (auto& p : g_placed) {
        auto mon = p.mon.lock();
        if (!mon)
            continue;
        lx = pos.x - mon->m_position.x - p.at.x;
        ly = pos.y - mon->m_position.y - p.at.y;
        if (lx >= 0 && ly >= 0 && lx < p.at.w && ly < p.at.h)
            return &p;
    }
    return nullptr;
}

int screenAt(const Vector2D& pos, PHLMONITOR* monOut = nullptr) {
    double lx = 0, ly = 0;
    const auto* p = pagerAt(pos, lx, ly);
    if (!p)
        return -1;
    if (monOut)
        *monOut = p->mon.lock();
    return pager::hitScreen(p->l, lx, ly);
}

// janela (a de cima) sob o ponto, dentro da tela `screen` do pager `p`
const Win* windowAt(const std::vector<Win>& ws, const Placed& p, int screen, double lx, double ly) {
    for (auto it = ws.rbegin(); it != ws.rend(); ++it) {
        if (it->ws != screen + 1)
            continue;
        const auto area = pager::fitMonitor(p.l.screens[screen], it->monW, it->monH);
        if (pager::thumb(area, it->monW, it->monH, it->box).contains(lx, ly))
            return &*it;
    }
    return nullptr;
}

void onMouseMove(const Vector2D& pos) {
    g_mouse = pos;
    if (g_press.pressed) {
        if (!g_press.dragging && !g_press.addr.empty() && std::hypot(pos.x - g_press.start.x, pos.y - g_press.start.y) > DRAG_START)
            g_press.dragging = true;
        if (g_press.dragging) {
            g_press.drop = screenAt(pos);
            for (auto& m : g_pCompositor->m_monitors) // o ícone segue o cursor (pode sair do pager)
                if (m && m->logicalBox().containsPoint(pos))
                    g_pHyprRenderer->damageMonitor(m);
            return;
        }
    }
    const int h = screenAt(pos);
    if (h != g_hover) {
        g_hover = h;
        damageAll();
    }
}

bool onMouseButton(const IPointer::SButtonEvent& ev) {
    if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        const bool s     = g_swallowRelease;
        g_swallowRelease = false;
        if (ev.button == BTN_LEFT_ && g_press.pressed) {
            const auto p = g_press;
            g_press      = {};
            if (p.dragging) {
                if (p.drop >= 0 && p.drop != p.screen)
                    moveTo(p.addr, p.drop + 1);
                for (auto& m : g_pCompositor->m_monitors)
                    if (m)
                        g_pHyprRenderer->damageMonitor(m);
            } else if (!p.addr.empty())
                hyprtabFocus(p.addr); // vai para o workspace dela e foca
            else
                switchTo(p.screen + 1);
            damageAll();
        }
        return s;
    }

    double      lx = 0, ly = 0;
    const auto* p = pagerAt(g_mouse, lx, ly);
    if (!p)
        return false;
    const int s = pager::hitScreen(p->l, lx, ly);
    if (s < 0)
        return false; // vãos entre as telas: o clique passa para a janela de baixo
    g_swallowRelease = true;
    if (ev.button == BTN_LEFT_) {
        const auto  ws = windows();
        const auto* w  = windowAt(ws, *p, s, lx, ly);
        g_press        = {true, false, p->mon, s, w ? w->addr : "", w ? w->cls : "", g_mouse, -1};
    }
    return true;
}

bool onKey(const IKeyboard::SKeyEvent& ev) {
    if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED || ev.keycode != KEY_ESC_ || !g_press.dragging)
        return false;
    g_press = {}; // Esc arrastando: cancela (o soltar continua engolido)
    for (auto& m : g_pCompositor->m_monitors)
        if (m)
            g_pHyprRenderer->damageMonitor(m);
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

void onAnim(SP<CEventLoopTimer> self, void*) {
    const double step  = ANIM_FRAME.count() / LAMP_MS;
    bool         again = false;
    for (auto& mon : g_pCompositor->m_monitors) {
        if (!mon)
            continue;
        auto it = g_lamps.find(mon->m_id);
        if (it == g_lamps.end())
            continue;
        const int ws = activeWorkspace(mon);
        for (size_t i = 0; i < it->second.size(); i++) {
            auto&        t  = it->second[i];
            const double to = (int)i + 1 == ws ? 1.0 : 0.0;
            t               = to > t ? std::min(to, t + step) : std::max(to, t - step);
            again           = again || t != to;
        }
    }
    damageAll();
    if (again)
        self->updateTimeout(ANIM_FRAME);
}

// ---------- leitura periódica ----------

std::string signature() {
    std::string s;
    for (auto& m : g_pCompositor->m_monitors)
        if (m)
            s += std::format("m{}:{};", m->m_id, activeWorkspace(m));
    for (auto& w : windows())
        s += std::format("{}:{}:{}:{}:{}:{}:{};", w.addr, w.ws, std::lround(w.box.x), std::lround(w.box.y), std::lround(w.box.w), std::lround(w.box.h), w.focused);
    return s;
}

void onPoll(SP<CEventLoopTimer> self, void*) {
    if (loadConfig()) {
        for (auto& m : g_pCompositor->m_monitors)
            if (m)
                g_pHyprRenderer->damageMonitor(m); // mudou de tamanho/lugar: some de onde estava
    }
    if (auto s = signature(); s != g_sig) {
        g_sig = std::move(s);
        damageAll();
        for (auto& m : g_pCompositor->m_monitors)
            if (m && lampsMoving(m))
                kickAnim(); // trocou de workspace: os refletores giram
    }
    self->updateTimeout(POLL_EVERY);
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    addConfig();
    loadConfig();
    registerEvents();
    render::registerRender(draw);

    g_poll = makeShared<CEventLoopTimer>(std::nullopt, onPoll, nullptr);
    g_pEventLoopManager->addTimer(g_poll);
    g_poll->updateTimeout(POLL_EVERY);
    g_anim = makeShared<CEventLoopTimer>(std::nullopt, onAnim, nullptr);
    g_pEventLoopManager->addTimer(g_anim);

    for (auto& m : g_pCompositor->m_monitors)
        if (m)
            g_pHyprRenderer->damageMonitor(m);

    return {"hyprpager", "Pager de workspaces no estilo dos temas metálicos clássicos do Enlightenment", "caio", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    for (auto* t : {&g_poll, &g_anim})
        if (*t) {
            g_pEventLoopManager->removeTimer(*t);
            t->reset();
        }
    unregisterEvents();
    render::unregisterRender();
    for (auto& m : g_pCompositor->m_monitors)
        if (m)
            g_pHyprRenderer->damageMonitor(m);
    g_placed.clear();
#ifdef HYPRPAGER_CFG_V2
    g_ints.clear();
    g_strs.clear();
#endif
}
