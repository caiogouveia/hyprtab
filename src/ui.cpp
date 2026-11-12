// Overlay do hyprtab desenhado dentro do compositor. O desenho em si (por era do Hyprland)
// fica em render.cpp; aqui só o layout da barra do switcher.

#include "ui.hpp"
#include "layout.hpp"
#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// dimensões lógicas (px de layout); multiplicadas pela escala do monitor ao desenhar
using namespace layout;
constexpr double RADIUS = 24, SEL_RADIUS = 18;

std::vector<ui::Item> g_items;
size_t                g_sel     = 0;
bool                  g_visible = false;
PHLMONITORREF         g_mon;

Geom geometry(const PHLMONITOR& mon) {
    return layout::geometry(g_items.size(), mon->m_size.x, mon->m_size.y);
}

void damage() {
    if (auto m = g_mon.lock())
        g_pHyprRenderer->damageMonitor(m);
}

void draw(const PHLMONITOR& mon) {
    if (!g_visible || g_items.empty() || mon != g_mon.lock() || hyprtabLocked())
        return;

    const double sc = mon->m_scale;
    const auto   g  = geometry(mon);
    auto         px = [&](double v) { return v * sc; };

    render::glassPanel(CBox{px(g.x), px(g.y), px(g.w), px(g.h)}, (int)px(RADIUS * g.k), sc, g.k);

    for (size_t i = 0; i < g_items.size(); i++) {
        const auto [cx, cy, cs] = cell(g, i);

        if (i == g_sel)
            render::fillRect(CBox{px(cx), px(cy), px(cs), px(cs)}, CHyprColor{1, 1, 1, 0.22}, (int)px(SEL_RADIUS * g.k));

        const int iconPx = std::max(16, (int)std::lround(px(ICON * g.k)));
        render::drawTexture(render::iconTexture(g_items[i].cls, iconPx), CBox{px(cx + PAD * g.k), px(cy + PAD * g.k), (double)iconPx, (double)iconPx});
    }

    const int    lw = std::max(32, (int)std::lround(px(g.w - 2 * BARPAD * g.k)));
    const int    lh = std::max(12, (int)std::lround(px(TITLE_H * g.k)));
    const auto&  it = g_items[std::min(g_sel, g_items.size() - 1)];
    const auto   tx = render::labelTexture(it.title.empty() ? it.cls : it.title, lw, lh);
    render::drawTexture(tx, CBox{px(g.x + BARPAD * g.k), px(g.y + g.h - (BARPAD * g.k) - TITLE_H * g.k), (double)lw, (double)lh});
}

} // namespace

namespace ui {

void init() {
    render::registerRender(draw);
}

void shutdown() {
    render::unregisterRender();
    g_visible = false;
}

void show(std::vector<Item> items, size_t selected) {
    g_items   = std::move(items);
    g_sel     = selected;
    g_visible = true;
    g_mon     = render::currentMonitor();
    damage();
}

void select(size_t selected) {
    if (selected == g_sel)
        return;
    g_sel = selected;
    damage();
}

void hide() {
    g_visible = false;
    damage();
}

bool visible() {
    return g_visible;
}

int hit(const Vector2D& global) {
    auto mon = g_mon.lock();
    if (!g_visible || !mon || g_items.empty())
        return -2;

    return layout::hit(geometry(mon), g_items.size(), global.x - mon->m_position.x, global.y - mon->m_position.y);
}

} // namespace ui
