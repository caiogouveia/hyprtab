#include "model.hpp"

#include <algorithm>

namespace borders {

static Shadow sane(Shadow s) {
    s.blur  = std::max(0.0, s.blur);
    s.alpha = std::clamp(s.alpha, 0.0, 1.0);
    return s;
}

std::array<Shadow, 2> shadowLayers(const Look& l, bool active) {
    return active ? std::array{sane(l.ambientActive), sane(l.contactActive)} : std::array{sane(l.ambientIdle), sane(l.contactIdle)};
}

ShadowDraw shadowDraw(const Rect& win, const Shadow& raw) {
    const Shadow s     = sane(raw);
    const double sigma = s.blur / 2;
    const double range = std::max(1.0, 4 * sigma);
    // miolo: a janela crescida pelo spread e encolhida por σ (nunca a ponto de sumir)
    const double grow  = std::max(s.spread - sigma, -std::min(win.w, win.h) / 2 + 1);
    const double out   = grow + range; // a caixa inteira em volta da janela
    return {{win.x - out, win.y - out + s.offsetY, win.w + 2 * out, win.h + 2 * out}, range, -grow};
}

Extents shadowExtents(const Look& l) {
    const double pad = 2;
    Extents      e;
    for (const Shadow& s : {l.ambientActive, l.ambientIdle, l.contactActive, l.contactIdle}) {
        // janela grande o bastante para o miolo não ser limitado
        const auto d   = shadowDraw({0, 0, 10000, 10000}, s);
        const double o = -d.box.x; // quanto passa de cada lado, antes do deslocamento
        e.top          = std::max(e.top, o - s.offsetY);
        e.bottom       = std::max(e.bottom, o + s.offsetY);
        e.left         = std::max(e.left, o);
        e.right        = std::max(e.right, o);
    }
    return {e.top + pad, e.right + pad, e.bottom + pad, e.left + pad};
}

} // namespace borders
