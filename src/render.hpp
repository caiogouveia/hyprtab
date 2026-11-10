#pragma once
#include "compat.hpp"

#include <cairo/cairo.h>
#include <string>

// Desenho dentro do compositor, compartilhado pelo switcher (hyprtab) e pela dock (hyprdock).
// A API de render muda por versão; tudo que muda fica isolado em render.cpp.
// Caixas e raios em px físicos do monitor (já multiplicados pela escala).
namespace render {

#ifdef HYPRTAB_NEW_RENDER
using Tex = SP<Render::ITexture>;
#else
using Tex = SP<CTexture>;
#endif

Tex  makeTexture(cairo_surface_t* s);
void fillRect(const CBox& b, const CHyprColor& c, int round, bool blur = false);
void drawTexture(const Tex& t, const CBox& b);

// texturas com cache (vivem até o fim do processo: destruir textura GL exige o contexto ativo)
Tex iconTexture(const std::string& cls, int px);
Tex labelTexture(const std::string& text, int w, int h, bool center = true); // texto branco, com reticências

// painel de vidro: sombra projetada + fundo translúcido com blur + borda dupla estilo macOS.
// `scale` é a escala do monitor; `k` reduz sombra e véu junto com o painel.
void glassPanel(const CBox& box, int round, double scale, double k = 1);

PHLMONITOR currentMonitor();

// chama `draw` no fim do render de cada monitor
void registerRender(void (*draw)(const PHLMONITOR&));
void unregisterRender();

} // namespace render
