#include "layout.hpp"

#include <algorithm>

namespace layout {

Geom geometry(size_t n, double monW, double monH) {
    const double cnt  = (double)std::max<size_t>(n, 1);
    const double cell = ICON + 2 * PAD;
    const double w    = cnt * cell + (cnt - 1) * GAP + 2 * BARPAD;
    const double h    = cell + TITLE_H + 2 * BARPAD;
    const double k    = std::min(1.0, (monW - 80) / w);
    return {(monW - w * k) / 2, (monH - h * k) / 2, w * k, h * k, k};
}

Cell cell(const Geom& g, size_t i) {
    const double size = (ICON + 2 * PAD) * g.k;
    return {g.x + BARPAD * g.k + (double)i * (size + GAP * g.k), g.y + BARPAD * g.k, size};
}

int hit(const Geom& g, size_t n, double lx, double ly) {
    if (lx < g.x || lx > g.x + g.w || ly < g.y || ly > g.y + g.h)
        return -2;
    for (size_t i = 0; i < n; i++) {
        const auto c = cell(g, i);
        if (lx >= c.x && lx <= c.x + c.size && ly >= c.y && ly <= c.y + c.size)
            return (int)i;
    }
    return -1;
}

} // namespace layout
