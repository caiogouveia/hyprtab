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

// reservar espaço na borda (como as barras): área dinâmica do monitor + layout recalculável.
// Na 0.57 o LayoutManager mudou de lugar e de API; lá a dock não reserva espaço.
#if __has_include(<hyprland/src/desktop/reserved/ReservedArea.hpp>) && __has_include(<hyprland/src/managers/LayoutManager.hpp>)
#include <hyprland/src/managers/LayoutManager.hpp>
#define HYPRTAB_RESERVE 1
#endif

#if __has_include(<hyprland/src/desktop/state/WindowState.hpp>)
#include <hyprland/src/desktop/state/WindowState.hpp>
#define HYPRTAB_WINDOW_STATE 1
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

inline void hyprtabFocus(const std::string& addr) {
    hyprtabDispatch(std::format("focuswindow address:{}", addr), std::format("hl.dsp.focus({{ window = 'address:{}' }})", addr));
}

// o workspace ativo do monitor tem janela em tela cheia? (membro mudou de forma entre versões)
template <class M>
inline bool hyprtabHasFullscreen(const M& mon) {
    if constexpr (requires { mon->m_activeWorkspace->m_hasFullscreenWindow; })
        return mon->m_activeWorkspace && mon->m_activeWorkspace->m_hasFullscreenWindow;
    else if constexpr (requires { mon->m_activeWorkspace->m_fullscreenMode; })
        return mon->m_activeWorkspace && mon->m_activeWorkspace->m_fullscreenMode != FSMODE_NONE;
    else
        return false;
}
