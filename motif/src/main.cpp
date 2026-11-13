// hyprmotif - molduras estilo Motif/mwm para o Hyprland (barra de título + borda em relevo).
// Alvo atual: Hyprland 0.53 (API de decorações e eventos do hyprbars v0.53.0).

#define WLR_USE_UNSTABLE

#include <any>
#include <algorithm>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleEffectContainer.hpp>

#include "frameDeco.hpp"
#include "globals.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static void onNewWindow(PHLWINDOW window) {
    if (window->m_X11DoesntWantBorders)
        return;
    if (std::ranges::any_of(window->m_windowDecorations, [](const auto& d) { return d->getDisplayName() == "HyprMotif"; }))
        return;

    auto frame = makeUnique<CMotifFrame>(window);
    g_pGlobalState->frames.emplace_back(frame);
    frame->m_self = frame;
    HyprlandAPI::addWindowDecoration(PHANDLE, window, std::move(frame));
}

static void onUpdateWindowRules(PHLWINDOW window) {
    const auto IT = std::ranges::find_if(g_pGlobalState->frames, [&](const auto& f) { return f && f->getOwner() == window; });
    if (IT == g_pGlobalState->frames.end())
        return;

    (*IT)->updateRules();
    window->updateWindowDecos();
}

// bind = SUPER, S, hyprmotif:shade  -> enrola/desenrola a janela ativa
static SDispatchResult dispShade(std::string) {
    const auto W = Desktop::focusState()->window();
    for (auto& f : g_pGlobalState->frames)
        if (f && f->getOwner() == W) {
            f->toggleShade();
            break;
        }
    return {};
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprmotif] Falha ao iniciar: versão dos headers diferente da do Hyprland em execução", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprmotif] Version mismatch");
    }

    g_pGlobalState                 = makeUnique<SGlobalState>();
    g_pGlobalState->noFrameRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprmotif:no_frame");

    static auto P1 = HyprlandAPI::registerCallbackDynamic(PHANDLE, "openWindow", [&](void*, SCallbackInfo&, std::any data) { onNewWindow(std::any_cast<PHLWINDOW>(data)); });
    static auto P2 = HyprlandAPI::registerCallbackDynamic(PHANDLE, "windowUpdateRules", [&](void*, SCallbackInfo&, std::any data) { onUpdateWindowRules(std::any_cast<PHLWINDOW>(data)); });
    static auto P3 = HyprlandAPI::registerCallbackDynamic(PHANDLE, "activeWindow", [&](void*, SCallbackInfo&, std::any) {
        for (auto& f : g_pGlobalState->frames)
            if (f)
                f->onFocusChanged();
    });
    static auto P4 = HyprlandAPI::registerCallbackDynamic(PHANDLE, "configReloaded", [&](void*, SCallbackInfo&, std::any) {
        for (auto& f : g_pGlobalState->frames)
            if (f)
                f->onConfigReloaded();
    });

    // cores padrão: cinza azulado do Motif
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:enabled", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:border_size", Hyprlang::INT{4});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:title_height", Hyprlang::INT{18});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:font", Hyprlang::STRING{"Sans Bold"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:font_size", Hyprlang::INT{10});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:col.active", Hyprlang::INT{*configStringToInt("rgba(aeb2c3ff)")});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:col.inactive", Hyprlang::INT{*configStringToInt("rgba(bcbcbcff)")});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:col.text", Hyprlang::INT{*configStringToInt("rgba(000000ff)")});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:col.text_inactive", Hyprlang::INT{*configStringToInt("rgba(404040ff)")});
    // sombras: 0 = automático (calculado a partir da face); como activeTopShadowColor etc. do mwm
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:col.active_top_shadow", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:col.active_bottom_shadow", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:col.inactive_top_shadow", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:col.inactive_bottom_shadow", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:title_enabled", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:title_align", Hyprlang::STRING{"center"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:double_click", Hyprlang::STRING{"maximize"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:on_minimize", Hyprlang::STRING{"movetoworkspacesilent special:minimized"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmotif:on_maximize", Hyprlang::STRING{"fullscreen 1"});

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprmotif:shade", dispShade);

    // janelas que já existiam antes do load
    for (auto& w : g_pCompositor->m_windows) {
        if (w->isHidden() || !w->m_isMapped)
            continue;
        onNewWindow(w);
    }

    HyprlandAPI::reloadConfig();

    return {"hyprmotif", "Molduras estilo Motif/mwm", "caio", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    for (auto& m : g_pCompositor->m_monitors)
        m->m_scheduledRecalc = true;

    g_pHyprRenderer->m_renderPass.removeAllOfType("CFramePassElement");

    Desktop::Rule::windowEffects()->unregisterEffect(g_pGlobalState->noFrameRuleIdx);
}
