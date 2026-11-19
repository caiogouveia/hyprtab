#include "model.hpp"

#include <algorithm>
#include <cctype>

namespace pager {

Layout layout(size_t n, double screenW) {
    Layout l;
    l.screenW = screenW;
    l.screenH = screenW / SCREEN_ASPECT;
    l.tube    = std::max(2.0, screenW * TUBE);

    const double gap  = screenW * GAP;
    const double left = l.tube * 2.2;            // espaço do engate à esquerda
    const double top  = gap * 0.55 + l.tube * 1.5; // espaço do cano que sobe da primeira tela
    const double bs   = screenW * BRACKET;
    for (size_t i = 0; i < n; i++) {
        const double y = top + (double)i * (l.screenH + gap);
        l.screens.push_back({left, y, screenW, l.screenH});
        const double cx = left + screenW + screenW * BRACKET_DX, cy = y + l.screenH * 0.3;
        l.brackets.push_back({cx - bs / 2, cy - bs / 2, bs, bs});
    }
    l.w = left + screenW + screenW * BRACKET_DX + bs * 1.05; // espaço para a traseira do refletor virado para a tela
    l.h = n ? l.screens.back().y + l.screenH + l.tube * 2 : top;
    return l;
}

namespace {
std::string lower(std::string s) {
    for (auto& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}
} // namespace

Edge parseEdge(const std::string& s) {
    return lower(s) == "right" ? Edge::Right : Edge::Left;
}

Align parseAlign(const std::string& s) {
    const std::string l = lower(s);
    return l == "top" ? Align::Top : l == "bottom" ? Align::Bottom : Align::Center;
}

Box place(const Layout& l, const Placement& p, double monW, double monH, const Reserved& r) {
    const double x = p.edge == Edge::Left ? r.left + p.margin : monW - r.right - p.margin - l.w;
    double       y = 0;
    switch (p.align) {
        case Align::Top: y = r.top + p.margin; break;
        case Align::Bottom: y = monH - r.bottom - p.margin - l.h; break;
        case Align::Center: y = r.top + (monH - r.top - r.bottom - l.h) / 2; break;
    }
    y = std::clamp(y + p.offset, 0.0, std::max(0.0, monH - l.h));
    return {x, y, l.w, l.h};
}

bool monitorMatches(const std::string& filter, const std::string& name) {
    bool any = false;
    for (size_t at = 0; at <= filter.size();) {
        size_t end = filter.find(',', at);
        if (end == std::string::npos)
            end = filter.size();
        std::string item = filter.substr(at, end - at);
        const auto  b    = item.find_first_not_of(" \t");
        item             = b == std::string::npos ? "" : item.substr(b, item.find_last_not_of(" \t") - b + 1);
        if (!item.empty()) {
            any = true;
            if (item == name)
                return true;
        }
        at = end + 1;
    }
    return !any;
}

int hitScreen(const Layout& l, double x, double y) {
    for (size_t i = 0; i < l.screens.size(); i++) {
        const auto& s = l.screens[i];
        const auto& b = l.brackets[i];
        // a moldura (cano em volta) também conta
        const Box frame{s.x - l.tube, s.y - l.tube, s.w + 2 * l.tube, s.h + 2 * l.tube};
        if (frame.contains(x, y) || b.contains(x, y))
            return (int)i;
    }
    return -1;
}

Box fitMonitor(const Box& glass, double monW, double monH) {
    const double pad = std::min(glass.w, glass.h) * 0.08;
    const Box    in{glass.x + pad, glass.y + pad, glass.w - 2 * pad, glass.h - 2 * pad};
    if (monW <= 0 || monH <= 0 || in.w <= 0 || in.h <= 0)
        return in;
    const double k = std::min(in.w / monW, in.h / monH);
    const double w = monW * k, h = monH * k;
    return {in.x + (in.w - w) / 2, in.y + (in.h - h) / 2, w, h};
}

Box thumb(const Box& area, double monW, double monH, const Box& win) {
    if (monW <= 0 || monH <= 0)
        return {area.x, area.y, 0, 0};
    const double kx = area.w / monW, ky = area.h / monH;
    double       x0 = area.x + win.x * kx, y0 = area.y + win.y * ky;
    double       x1 = x0 + win.w * kx, y1 = y0 + win.h * ky;
    x0              = std::clamp(x0, area.x, area.x + area.w);
    x1              = std::clamp(x1, area.x, area.x + area.w);
    y0              = std::clamp(y0, area.y, area.y + area.h);
    y1              = std::clamp(y1, area.y, area.y + area.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

std::string workspaceLabel(int id, const std::string& name) {
    return name.empty() || name == std::to_string(id) ? "" : name;
}

} // namespace pager
