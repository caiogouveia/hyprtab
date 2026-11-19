#pragma once
// Desenho da moldura (anel, bisel, botões) sobre uma "tela" abstrata de retângulos chapados.
// O plugin implementa a tela com o GL do Hyprland; tools/preview.cpp, com cairo. Sem dependência do Hyprland.

#include "geometry.hpp"

#include <optional>

namespace motif {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};

// face + bisel claro (cima/esquerda) e escuro (baixo/direita)
struct Palette {
    Color face, light, dark;
};

Color   lighter(Color c);
Color   darker(Color c);
// sombras ausentes saem da face (como o mwm faz a partir do background)
Palette makePalette(Color face, std::optional<Color> top, std::optional<Color> bottom);

struct Canvas {
    virtual ~Canvas()                                = default;
    virtual void fill(const Rect& r, const Color& c) = 0; // r relativo ao canto da moldura, px lógicos
};

struct PaintState {
    Part pressed = Part::None; // botão apertado (afundado)
    bool shaded  = false;      // enrolada: o cliente é coberto pela face
};

void paintFrame(Canvas& cv, const Layout& L, double w, double h, int border, int title, const Palette& pal, const PaintState& st);

} // namespace motif
