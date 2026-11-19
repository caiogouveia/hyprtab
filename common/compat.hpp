#pragma once
// Detecção de era do Hyprland por __has_include (0.52 -> 0.57+).
//
//   HYPRTAB_EVENT_BUS    >= 0.54  eventos via Event::bus() (registerCallbackDynamic virou no-op)
//   HYPRTAB_FOCUS_STATE  >= 0.53  Desktop::focusState()
//   HYPRTAB_NEW_RENDER   >= 0.55  ITexture + g_pHyprRenderer->createTexture(cairo)
//   HYPRTAB_RENDER_CTX   >= 0.57  renderRect/renderTexture recebem CRenderContext&
//   HYPRTAB_WINDOW_STATE >= 0.57  lista de janelas em Desktop::windowState() (saiu do CCompositor)
//   HYPRTAB_RESERVE      0.53-0.56 dá para reservar espaço na borda (ReservedArea + LayoutManager)

#define WLR_USE_UNSTABLE

#include <cstdint>
#include <format>
#include <string>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>

// header da janela: desktop/ -> desktop/view/ -> desktop/view/window/
#if __has_include(<hyprland/src/desktop/view/window/Window.hpp>)
#include <hyprland/src/desktop/view/window/Window.hpp>
#elif __has_include(<hyprland/src/desktop/view/Window.hpp>)
#include <hyprland/src/desktop/view/Window.hpp>
#else
#include <hyprland/src/desktop/Window.hpp>
#endif

// header do monitor: helpers/ -> output/
#if __has_include(<hyprland/src/output/Monitor.hpp>)
#include <hyprland/src/output/Monitor.hpp>
#else
#include <hyprland/src/helpers/Monitor.hpp>
#endif

#if __has_include(<hyprland/src/event/EventBus.hpp>)
#define HYPRTAB_EVENT_BUS 1
#endif

#if __has_include(<hyprland/src/desktop/state/FocusState.hpp>)
#include <hyprland/src/desktop/state/FocusState.hpp>
#define HYPRTAB_FOCUS_STATE 1
#endif

#include <hyprland/src/Compositor.hpp>
#if __has_include(<hyprland/src/managers/SessionLockManager.hpp>)
#include <hyprland/src/managers/SessionLockManager.hpp>
#endif

// reservar espaço na borda (como as barras): área dinâmica do monitor + layout recalculável.
// Na 0.57 o LayoutManager mudou de lugar e de API; lá a dock não reserva espaço.
#if __has_include(<hyprland/src/desktop/reserved/ReservedArea.hpp>) && __has_include(<hyprland/src/managers/LayoutManager.hpp>)
#include <hyprland/src/managers/LayoutManager.hpp>
#define HYPRTAB_RESERVE 1
#endif

#if __has_include(<hyprland/src/managers/fullscreen/FullscreenController.hpp>)
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#define HYPRTAB_FS_CONTROLLER 1 // >= 0.57: tela cheia saiu do workspace
#endif

#if __has_include(<hyprland/src/desktop/state/WindowState.hpp>)
#include <hyprland/src/desktop/state/WindowState.hpp>
#define HYPRTAB_WINDOW_STATE 1
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#endif

#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Texture.hpp>

#if __has_include(<hyprland/src/render/GLRenderer.hpp>)
#define HYPRTAB_NEW_RENDER 1
#endif

#if __has_include(<hyprland/src/render/Context.hpp>)
#include <hyprland/src/render/Context.hpp>
#define HYPRTAB_RENDER_CTX 1
#endif

inline HANDLE PHANDLE = nullptr;

// onde mora o objeto do renderer OpenGL
#if defined(HYPRTAB_RENDER_CTX)
#define HYPRTAB_GL (Render::GL::g_pHyprOpenGL)
#elif defined(HYPRTAB_NEW_RENDER)
#define HYPRTAB_GL (Render::g_pHyprOpenGL)
#else
#define HYPRTAB_GL (g_pHyprOpenGL)
#endif

// janela mapeada: m_isMapped virou privado na 0.57 (use mapped())
template <class W>
inline bool hyprtabMapped(const W& w) {
    if constexpr (requires { w->mapped(); })
        return w->mapped();
    else
        return w->m_isMapped;
}

// todas as janelas do compositor
inline const std::vector<PHLWINDOW>& hyprtabWindows() {
#ifdef HYPRTAB_WINDOW_STATE
    return Desktop::windowState()->windows();
#else
    return g_pCompositor->m_windows;
#endif
}

// janela em foco
inline PHLWINDOW hyprtabFocusedWindow() {
#ifdef HYPRTAB_FOCUS_STATE
    return Desktop::focusState()->window();
#else
    return g_pCompositor->m_lastWindow.lock();
#endif
}

// endereço no mesmo formato do `hyprctl clients` ("0x...")
inline std::string hyprtabAddr(const PHLWINDOW& w) {
    return std::format("0x{:x}", (uintptr_t)w.get());
}

// `hyprctl dispatch` muda de sintaxe: hyprlang usa "focuswindow address:X";
// com config Lua (0.57+) a string vira hl.dispatch(<expressão Lua>).
// Tenta o formato legado e, se não deu "ok", o Lua. O formato errado só gera erro inofensivo.
inline void hyprtabDispatch(const std::string& legacy, const std::string& lua) {
    const auto r = HyprlandAPI::invokeHyprctlCommand("dispatch", legacy);
    if (!r.starts_with("ok"))
        HyprlandAPI::invokeHyprctlCommand("dispatch", lua);
}

// traz a janela para cima das outras. O focuswindow do Hyprland só foca: uma janela flutuante continua
// atrás das que estavam por cima dela. (Janela lado a lado já fica sempre abaixo das flutuantes.)
template <class W>
inline void hyprtabRaise(const W& w) {
    if (!w)
        return;
#ifdef HYPRTAB_WINDOW_STATE
    (void)Config::Actions::alterZOrder("top", w);
#else
    g_pCompositor->changeWindowZOrder(w, true);
#endif
}

inline void hyprtabFocus(const std::string& addr) {
    hyprtabDispatch(std::format("focuswindow address:{}", addr), std::format("hl.dsp.focus({{ window = 'address:{}' }})", addr));
    // o focuswindow é síncrono: a janela em foco agora é a pedida
    if (auto w = hyprtabFocusedWindow(); w && hyprtabAddr(w) == addr)
        hyprtabRaise(w);
}

// tela bloqueada (hyprlock etc.)? O render em RENDER_LAST_MOMENT fica por cima do lock, então quem
// desenha precisa checar isso
inline bool hyprtabLocked() {
#if __has_include(<hyprland/src/managers/SessionLockManager.hpp>)
    return g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();
#else
    return false;
#endif
}

// o workspace ativo do monitor tem janela em tela cheia de verdade (cobre o monitor inteiro)?
// Maximizada (`fullscreen 1`) não conta: ela respeita a área reservada, e esconder dock/barra deixaria
// a faixa reservada vazia. (Os membros mudaram de forma entre versões.)
template <class M>
inline bool hyprtabHasFullscreen(const M& mon) {
#ifdef HYPRTAB_FS_CONTROLLER
    return Fullscreen::controller()->hasFullscreen(mon);
#else
    if constexpr (requires { mon->m_activeWorkspace->m_fullscreenMode, mon->m_activeWorkspace->m_hasFullscreenWindow; })
        return mon->m_activeWorkspace && mon->m_activeWorkspace->m_hasFullscreenWindow && (mon->m_activeWorkspace->m_fullscreenMode & FSMODE_FULLSCREEN);
    else
        return mon->m_activeWorkspace && (mon->m_activeWorkspace->m_fullscreenMode & FSMODE_FULLSCREEN);
#endif
}

// ---- acessores que mudaram de forma entre versões (0.53 -> 0.57) ----
// Templates sobre o tipo da janela: o `requires` só é avaliado na instanciação, então as duas formas
// convivem sem erro de membro inexistente.
template <class W>
inline std::string hyprtabClass(const W& w) {
    if constexpr (requires { w->metadata().appID(); })
        return w->metadata().appID();
    else
        return w->m_class;
}
template <class W>
inline std::string hyprtabInitialClass(const W& w) {
    if constexpr (requires { w->metadata().initialAppID(); })
        return w->metadata().initialAppID();
    else
        return w->m_initialClass;
}
template <class W>
inline std::string hyprtabTitle(const W& w) {
    if constexpr (requires { w->metadata().title(); })
        return w->metadata().title();
    else
        return w->m_title;
}
template <class W>
inline bool hyprtabFloating(const W& w) {
    if constexpr (requires { w->isFloating(); })
        return w->isFloating();
    else
        return w->m_isFloating;
}
template <class W>
inline bool hyprtabPinned(const W& w) {
#ifdef HYPRTAB_WINDOW_STATE
    return (w->m_state & Desktop::View::WINDOW_STATE_PINNED) == Desktop::View::WINDOW_STATE_PINNED;
#else
    return w->m_pinned;
#endif
}
// id numérico do workspace (0 para workspace especial)
template <class WS>
inline int hyprtabWsId(const WS& ws) {
    if (!ws)
        return 0;
    if constexpr (requires { ws->numberedID(); })
        return (int)ws->numberedID().value_or(0);
    else
        return (int)ws->m_id;
}
template <class W>
inline int hyprtabWorkspaceId(const W& w) {
    if constexpr (requires { w->workspaceID(); })
        return (int)w->workspaceID();
    else
        return hyprtabWsId(w->m_workspace);
}
template <class W>
inline bool hyprtabIsFullscreen(const W& w) {
#ifdef HYPRTAB_FS_CONTROLLER
    return Fullscreen::controller()->isFullscreen(w);
#else
    return w->isFullscreen();
#endif
}
// id da janela X11 (0 se for Wayland nativa)
template <class W>
inline uint32_t hyprtabXid(const W& w) {
    if (!w)
        return 0;
    if constexpr (requires { w->backend().isX11(); }) {
        if (!w->backend().isX11())
            return 0;
        return (uint32_t)w->backend().clientID().id;
    } else {
        if (!w->m_isX11)
            return 0;
        const auto xs = w->m_xwaylandSurface.lock();
        return xs ? xs->m_xID : 0;
    }
}

// monitores ativos
#if __has_include(<hyprland/src/state/MonitorState.hpp>)
#include <hyprland/src/state/MonitorState.hpp>
inline const std::vector<PHLMONITOR>& hyprtabMonitors() {
    return State::monitorState()->monitors();
}
#else
inline const std::vector<PHLMONITOR>& hyprtabMonitors() {
    return g_pCompositor->m_monitors;
}
#endif

// comando do hyprctl (`hyprctl <nome>`): a API mudou na 0.57 (IPC::Socket1)
#if __has_include(<hyprland/src/ipc/s1/S1.hpp>)
#include <hyprland/src/ipc/s1/S1.hpp>
using HyprtabCtl = SP<IPC::Socket1::SCommand>;
inline HyprtabCtl hyprtabRegisterCtl(const std::string& name, std::function<std::string()> fn) {
    return HyprlandAPI::registerHyprCtlCommand(PHANDLE, IPC::Socket1::SCommand{name, IPC::Socket1::COMMAND_MATCH_EXACT,
                                                                               [fn](const IPC::Socket1::SRequest&) { return IPC::Socket1::SResponse(fn()); }});
}
#else
using HyprtabCtl = SP<SHyprCtlCommand>;
inline HyprtabCtl hyprtabRegisterCtl(const std::string& name, std::function<std::string()> fn) {
    return HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{name, true, [fn](eHyprCtlOutputFormat, std::string) { return fn(); }});
}
#endif

// tela cheia de verdade na janela (não maximizada)
template <class W>
inline bool hyprtabRealFullscreen(const W& w) {
#ifdef HYPRTAB_FS_CONTROLLER
    return Fullscreen::controller()->isFullscreen(w, Fullscreen::FSMODE_FULLSCREEN);
#else
    return w->isFullscreen() && w->m_workspace && (w->m_workspace->m_fullscreenMode & FSMODE_FULLSCREEN);
#endif
}
