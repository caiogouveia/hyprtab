#include "paint.hpp"

#include <algorithm>
#include <cmath>

namespace motif {

Color lighter(Color c) {
    return {c.r + (1.F - c.r) * 0.55F, c.g + (1.F - c.g) * 0.55F, c.b + (1.F - c.b) * 0.55F, c.a};
}
Color darker(Color c) {
    return {c.r * 0.45F, c.g * 0.45F, c.b * 0.45F, c.a};
}

Palette makePalette(Color face, std::optional<Color> top, std::optional<Color> bottom) {
    return {face, top.value_or(lighter(face)), bottom.value_or(darker(face))};
}

void paintFrame(Canvas& cv, const Layout& L, double w, double h, int border, int title, const Palette& pal, const PaintState& st) {
    const double B = border, T = title;

    // bisel de `bw` px em volta de `r`: raised = claro em cima/esquerda, escuro embaixo/direita
    // (sunken inverte). As bordas escuras vão primeiro e as claras cobrem os cantos.
    auto bevel = [&](const Rect& r, double bw, bool isRaised) {
        const auto& tl = isRaised ? pal.light : pal.dark;
        const auto& br = isRaised ? pal.dark : pal.light;
        cv.fill({r.x, r.y + r.h - bw, r.w, bw}, br);
        cv.fill({r.x + r.w - bw, r.y, bw, r.h}, br);
        cv.fill({r.x, r.y, r.w - bw, bw}, tl);
        cv.fill({r.x, r.y, bw, r.h - bw}, tl);
    };
    auto raised = [&](const Rect& r, double bw, bool isRaised = true) {
        cv.fill(r, pal.face);
        bevel(r, bw, isRaised);
    };

    // 1) anel da moldura (face) + bisel externo de 2 px
    cv.fill({0, 0, w, B + T}, pal.face);
    cv.fill({0, h - B, w, B}, pal.face);
    cv.fill({0, B + T, B, L.client.h}, pal.face);
    cv.fill({w - B, B + T, B, L.client.h}, pal.face);
    if (border >= 2)
        bevel({0, 0, w, h}, 2, true);

    if (st.shaded)
        cv.fill(L.client, pal.face);

    // 2) friso "afundado" em volta do cliente, dentro da borda
    if (border >= 3 && L.client.w > 0 && L.client.h > 0)
        bevel({L.client.x - 1, L.client.y - 1, L.client.w + 2, L.client.h + 2}, 1, false);

    // 3) barra de título
    if (title >= 2 && L.bar.w > 0)
        raised(L.bar, 1);

    // 4) botões: quadrado em relevo (afundado enquanto apertado) com um glifo
    auto button = [&](const Rect& cell, Part which) {
        if (cell.w <= 0)
            return;
        const bool down = st.pressed == which;
        const Rect r    = cell.inset(1);
        raised(r, 1, !down);

        const double off = down ? 1 : 0; // glifo desce 1 px quando apertado
        const double cx = r.x + r.w / 2.0 + off, cy = r.y + r.h / 2.0 + off;
        auto         glyph = [&](double gw, double gh) {
            gw = std::floor(gw), gh = std::floor(gh);
            raised({std::floor(cx - gw / 2), std::floor(cy - gh / 2), gw, gh}, 1);
        };
        switch (which) {
            case Part::Menu: glyph(std::max(6.0, r.w * 0.5), std::max(3.0, r.h * 0.2)); break;       // traço
            case Part::Minimize: glyph(std::max(4.0, r.w * 0.3), std::max(4.0, r.w * 0.3)); break;   // quadradinho
            case Part::Maximize: glyph(std::max(7.0, r.w * 0.6), std::max(7.0, r.w * 0.6)); break;   // quadrado grande
            default: break;
        }
    };
    button(L.menu, Part::Menu);
    button(L.minimize, Part::Minimize);
    button(L.maximize, Part::Maximize);
}

} // namespace motif
