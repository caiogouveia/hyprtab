#include "geometry.hpp"

#include <algorithm>

namespace motif {

Layout layout(double w, double h, int border, int title) {
    const double B = border, T = title;
    Layout       l;

    l.bar    = {B, B, std::max(0.0, w - 2 * B), T};
    l.client = {B, B + T, std::max(0.0, w - 2 * B), std::max(0.0, h - 2 * B - T)};

    // os botões saem por falta de espaço: primeiro min/max, depois o menu
    const bool hasMenu = l.bar.w >= T;
    const bool hasMinMax = l.bar.w >= 4 * T;

    double textL = l.bar.x, textR = l.bar.x + l.bar.w;
    if (hasMenu) {
        l.menu = {l.bar.x, l.bar.y, T, T};
        textL  = l.menu.x + T;
    }
    if (hasMinMax) {
        l.maximize = {l.bar.x + l.bar.w - T, l.bar.y, T, T};
        l.minimize = {l.maximize.x - T, l.bar.y, T, T};
        textR      = l.minimize.x;
    }
    l.text = {textL, l.bar.y, std::max(0.0, textR - textL), T};
    return l;
}

Part hit(const Layout& l, double w, double h, double x, double y) {
    if (x < 0 || y < 0 || x >= w || y >= h)
        return Part::None;
    if (l.client.contains(x, y))
        return Part::Client;
    if (l.menu.contains(x, y))
        return Part::Menu;
    if (l.minimize.contains(x, y))
        return Part::Minimize;
    if (l.maximize.contains(x, y))
        return Part::Maximize;
    if (l.bar.contains(x, y))
        return Part::Title;
    return Part::Frame;
}

} // namespace motif
