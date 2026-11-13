#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <string>
#include <vector>

inline HANDLE PHANDLE = nullptr;

class CMotifFrame;

struct SGlobalState {
    std::vector<WP<CMotifFrame>> frames;
    uint32_t                     noFrameRuleIdx = 0; // windowrule "hyprmotif:no_frame"
};

inline UP<SGlobalState> g_pGlobalState;

// ---- config (plugin:hyprmotif:*) ----
// Ponteiros estáticos do Hyprlang: o valor acompanha o hyprland.conf sem precisar reler.
namespace cfg {

#define MOTIF_INT(name, key)                                                                                           \
    inline long name() {                                                                                               \
        static auto* const P = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprmotif:" key)->getDataStaticPtr(); \
        return **P;                                                                                                    \
    }
#define MOTIF_STR(name, key)                                                                                           \
    inline std::string name() {                                                                                        \
        static auto* const P = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprmotif:" key)->getDataStaticPtr(); \
        return *P;                                                                                                     \
    }

MOTIF_INT(enabled, "enabled")
MOTIF_INT(border, "border_size")
MOTIF_INT(titleHeight, "title_height")
MOTIF_INT(fontSize, "font_size")
MOTIF_INT(colActive, "col.active")
MOTIF_INT(colInactive, "col.inactive")
MOTIF_INT(colText, "col.text")
MOTIF_INT(colTextInactive, "col.text_inactive")
MOTIF_INT(colActiveTop, "col.active_top_shadow")
MOTIF_INT(colActiveBottom, "col.active_bottom_shadow")
MOTIF_INT(colInactiveTop, "col.inactive_top_shadow")
MOTIF_INT(colInactiveBottom, "col.inactive_bottom_shadow")
MOTIF_INT(titleEnabled, "title_enabled")
MOTIF_STR(titleAlign, "title_align")
MOTIF_STR(doubleClick, "double_click")
MOTIF_STR(font, "font")
MOTIF_STR(onMinimize, "on_minimize")
MOTIF_STR(onMaximize, "on_maximize")

#undef MOTIF_INT
#undef MOTIF_STR

} // namespace cfg
