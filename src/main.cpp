// hyprtab - alt+tab estilo macOS para o Hyprland (0.52 até 0.57+), tudo dentro do .so.
//
// Compatibilidade entre versões (detecção por __has_include em compat.hpp):
//   <= 0.53 : eventos via HyprlandAPI::registerCallbackDynamic
//   >= 0.54 : registerCallbackDynamic virou no-op; eventos via Event::bus()
//   >= 0.57 : addDispatcherV2 virou no-op; dispatchers pela config Lua (addLuaFunction)

#include "compat.hpp"
#include "clients.hpp"
#include "ui.hpp"

#include <algorithm>
#include <any>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

// códigos evdev e de botão
static constexpr uint32_t KEY_ESC_   = 1;
static constexpr uint32_t KEY_ENTER_ = 28;
static constexpr uint32_t KEY_LALT_  = 56;
static constexpr uint32_t KEY_RALT_  = 100;
static constexpr uint32_t KEY_LEFT_  = 105;
static constexpr uint32_t KEY_RIGHT_ = 106;
static constexpr uint32_t BTN_LEFT_  = 0x110;

namespace {

std::vector<PHLWINDOWREF> g_mru;     // mais recente primeiro
std::vector<PHLWINDOWREF> g_session; // snapshot congelado enquanto o switcher está aberto
bool                      g_active = false;
size_t                    g_index  = 0;

// mouse: o hover só vale depois que o cursor realmente se mexe (a barra costuma abrir debaixo dele)
bool     g_haveAnchor   = false;
Vector2D g_anchor       = {};
bool     g_moved        = false;
bool     g_swallowClick = false; // engole o release do clique que já consumimos

// ---------- util ----------

std::string addr(const PHLWINDOW& w) {
    return std::format("0x{:x}", (uintptr_t)w.get());
}

// `hyprctl dispatch` muda de sintaxe: hyprlang usa "focuswindow address:X";
// com config Lua (0.57+) a string vira hl.dispatch(<expressão Lua>).
// Tenta o formato legado e, se não deu "ok", o Lua. O formato errado só gera erro inofensivo.
void dispatch(const std::string& legacy, const std::string& lua) {
    const auto r = HyprlandAPI::invokeHyprctlCommand("dispatch", legacy);
    if (!r.starts_with("ok"))
        HyprlandAPI::invokeHyprctlCommand("dispatch", lua);
}

// ---------- classe/título via `hyprctl clients -j` (estável entre versões) ----------

Clients fetchClients() {
    return parseClients(HyprlandAPI::invokeHyprctlCommand("clients", "", "j"));
}

std::vector<ui::Item> buildItems() {
    const auto            clients = fetchClients();
    std::vector<ui::Item> items;
    for (auto& r : g_session) {
        auto w = r.lock();
        if (!w)
            continue;
        const auto a = addr(w);
        ui::Item   it{a, "", ""};
        if (auto c = clients.find(a); c != clients.end())
            it.cls = c->second.first, it.title = c->second.second;
        items.push_back(std::move(it));
    }
    return items;
}

// ---------- lógica ----------

void pruneMru() {
    std::erase_if(g_mru, [](const PHLWINDOWREF& w) {
        auto l = w.lock();
        return !l || !hyprtabMapped(l);
    });
}

void touch(PHLWINDOW w) {
    if (!w)
        return;
    std::erase_if(g_mru, [&](const PHLWINDOWREF& r) { return r.lock() == w; });
    g_mru.insert(g_mru.begin(), w);
}

PHLWINDOW selected() {
    return g_index < g_session.size() ? g_session[g_index].lock() : nullptr;
}

void begin() {
    pruneMru();
    g_session = g_mru;
    if (g_session.size() < 2)
        return;
    g_active       = true;
    g_index        = 0;
    g_haveAnchor   = false;
    g_moved        = false;
    g_swallowClick = false;
}

void step(int dir) {
    const bool opening = !g_active;
    if (opening)
        begin();
    if (!g_active)
        return;
    const int n = (int)g_session.size();
    g_index     = (size_t)(((int)g_index + dir % n + n) % n);
    if (opening)
        ui::show(buildItems(), g_index);
    else
        ui::select(g_index);
}

void finish() {
    g_active = false;
    ui::hide();
}

void commit() {
    if (!g_active)
        return;
    auto w = selected();
    finish();
    if (w)
        dispatch(std::format("focuswindow address:{}", addr(w)), std::format("hl.dsp.focus({{ window = 'address:{}' }})", addr(w)));
}

void cancel() {
    if (g_active)
        finish();
}

// retorna true se a tecla deve ser consumida
bool handleKey(const IKeyboard::SKeyEvent& ev) {
    if (!g_active)
        return false;

    const bool pressed = ev.state == WL_KEYBOARD_KEY_STATE_PRESSED;

    if (!pressed && (ev.keycode == KEY_LALT_ || ev.keycode == KEY_RALT_)) {
        commit(); // não consome: o Alt solto precisa chegar ao sistema
        return false;
    }
    if (!pressed)
        return false;

    switch (ev.keycode) {
        case KEY_ESC_: cancel(); return true;
        case KEY_LEFT_: step(-1); return true;
        case KEY_RIGHT_: step(+1); return true;
        case KEY_ENTER_: commit(); return true;
        default: return false;
    }
}

// Mouse: não consumimos o movimento (o cursor precisa continuar andando).
void handleMouseMove(const Vector2D& pos) {
    if (!g_active)
        return;
    if (!g_haveAnchor) {
        g_haveAnchor = true;
        g_anchor     = pos;
        return;
    }
    if (!g_moved && std::abs(pos.x - g_anchor.x) + std::abs(pos.y - g_anchor.y) < 6)
        return;
    g_moved = true;

    const int i = ui::hit(pos);
    if (i >= 0 && (size_t)i != g_index) {
        g_index = (size_t)i;
        ui::select(g_index);
    }
}

void handleActive(PHLWINDOW w) {
    if (!g_active) // não reordena o MRU enquanto o switcher está aberto
        touch(w);
}

SDispatchResult dispNext(std::string) {
    step(+1);
    return {};
}
SDispatchResult dispPrev(std::string) {
    step(-1);
    return {};
}

// Callbacks Lua (config Lua, 0.57+): hl.plugin.hyprtab.next() / .prev().
// `auto*` evita nomear lua_State, que não existe nos headers antigos.
int luaNext(auto*) {
    step(+1);
    return 0;
}
int luaPrev(auto*) {
    step(-1);
    return 0;
}

// <= 0.56: dispatchers clássicos (bind = ALT, TAB, hyprtab:next).
// >= 0.57: addDispatcherV2 virou no-op; usa addLuaFunction (existe desde a 0.55).
// O `requires` dentro de lambda genérica deixa compilar nas versões sem addLuaFunction.
// A chamada tem de ser não qualificada (via using namespace): `HyprlandAPI::addLuaFunction`
// é procurado já na definição e dá erro onde não existe; com argumento dependente, só na instanciação.
void registerDispatchers() {
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprtab:next", dispNext);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprtab:prev", dispPrev);

    [](auto h) {
        using namespace HyprlandAPI;
        if constexpr (requires { addLuaFunction(h, "hyprtab", "next", luaNext); }) {
            addLuaFunction(h, "hyprtab", "next", luaNext);
            addLuaFunction(h, "hyprtab", "prev", luaPrev);
        }
    }(PHANDLE);
}

// ---------- registro de eventos (por era) ----------

Vector2D g_lastMouse = {}; // última posição global conhecida

bool onMouseButton(const IPointer::SButtonEvent& ev) {
    if (!g_active)
        return false;

    if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        if (g_swallowClick) {
            g_swallowClick = false;
            return true;
        }
        return false;
    }

    g_swallowClick = true; // engole também o release
    if (ev.button != BTN_LEFT_) {
        cancel();
        return true;
    }

    const int i = ui::hit(g_lastMouse);
    if (i >= 0) {
        g_index = (size_t)i;
        commit();
    } else {
        cancel(); // clique fora da barra (ou entre ícones) dispensa o switcher
    }
    return true;
}

void onMouseMove(const Vector2D& pos) {
    g_lastMouse = pos;
    handleMouseMove(pos);
}

#ifdef HYPRTAB_EVENT_BUS
CHyprSignalListener g_keyListener, g_activeListener, g_moveListener, g_buttonListener;

void registerEvents() {
    auto& ev = Event::bus()->m_events;
    g_keyListener = ev.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) {
        if (handleKey(e))
            info.cancelled = true;
    });
    g_activeListener = ev.window.active.listen([](PHLWINDOW w, Desktop::eFocusReason) { handleActive(w); });
    g_moveListener   = ev.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo&) { onMouseMove(pos); });
    g_buttonListener = ev.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
        if (onMouseButton(e))
            info.cancelled = true;
    });
}
void unregisterEvents() {
    g_keyListener.reset();
    g_activeListener.reset();
    g_moveListener.reset();
    g_buttonListener.reset();
}
#else
SP<HOOK_CALLBACK_FN> g_keyCb, g_activeCb, g_moveCb, g_buttonCb;

void registerEvents() {
    g_keyCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "keyPress", [](void*, SCallbackInfo& info, std::any data) {
        try {
            auto map = std::any_cast<std::unordered_map<std::string, std::any>>(data);
            if (handleKey(std::any_cast<IKeyboard::SKeyEvent>(map["event"])))
                info.cancelled = true;
        } catch (...) {}
    });
    g_activeCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "activeWindow", [](void*, SCallbackInfo&, std::any data) {
        try {
            handleActive(std::any_cast<PHLWINDOW>(data));
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
    g_activeCb.reset();
    g_moveCb.reset();
    g_buttonCb.reset();
}
#endif

PHLWINDOW currentFocus() {
#ifdef HYPRTAB_FOCUS_STATE
    return Desktop::focusState()->window();
#else
    return g_pCompositor->m_lastWindow.lock();
#endif
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    registerDispatchers();
    registerEvents();
    ui::init();
    // janelas que já existiam antes do load entram no MRU (ordem do compositor); a focada vai pra frente
    for (auto& w : g_pCompositor->m_windows)
        if (hyprtabMapped(w))
            g_mru.push_back(w);
    touch(currentFocus());

    return {"hyprtab", "Alt+Tab estilo macOS", "caio", "0.3.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    cancel();
    unregisterEvents();
    ui::shutdown();
}
