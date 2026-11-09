// Overlay do hyprtab desenhado dentro do compositor.
// A API de render muda por versão; tudo que muda fica isolado nas funções
// fillRect / drawTexture / makeTexture / registerRender abaixo.

#include "ui.hpp"
#include "icons.hpp"

#include <drm_fourcc.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#ifdef HYPRTAB_NEW_RENDER
using Tex = SP<Render::ITexture>;
#else
using Tex = SP<CTexture>;
#endif

namespace {

// dimensões lógicas (px de layout); multiplicadas pela escala do monitor ao desenhar
constexpr double ICON = 96, PAD = 14, GAP = 6, BARPAD = 18, TITLE_H = 34, RADIUS = 24, SEL_RADIUS = 18;
// sombra projetada: alcance do blur e deslocamento vertical; alfa máximo do preto
constexpr double SHADOW_RANGE = 44, SHADOW_DY = 12, SHADOW_A = 0.55;
// fundo da barra: translúcido para o blur do compositor aparecer por trás
constexpr double BAR_ALPHA = 0.55;

std::vector<ui::Item> g_items;
size_t                g_sel     = 0;
bool                  g_visible = false;
PHLMONITORREF         g_mon;

#ifdef HYPRTAB_RENDER_CTX
Render::CRenderContext* g_ctx = nullptr; // válido só durante o callback de render
#endif

// caches vivem até o fim do processo de propósito: destruir texturas GL exige o contexto
// ativo, o que não temos em PLUGIN_EXIT.
std::unordered_map<std::string, Tex>& iconCache() {
    static auto* c = new std::unordered_map<std::string, Tex>();
    return *c;
}
struct Label {
    std::string text;
    int         w = 0, h = 0;
    Tex         tex;
};
Label& labelCache() {
    static auto* l = new Label();
    return *l;
}

struct Shadow {
    int w = 0, h = 0, round = 0, range = 0;
    Tex tex;
};
Shadow& shadowCache() {
    static auto* s = new Shadow();
    return *s;
}

struct Geom {
    double x, y, w, h; // barra, em coords lógicas locais ao monitor
    double k;          // fator de redução quando há muitas janelas
};

Geom geometry(const PHLMONITOR& mon) {
    const double n    = (double)std::max<size_t>(g_items.size(), 1);
    const double cell = ICON + 2 * PAD;
    const double w    = n * cell + (n - 1) * GAP + 2 * BARPAD;
    const double h    = cell + TITLE_H + 2 * BARPAD;
    const double k    = std::min(1.0, (mon->m_size.x - 80) / w);
    return {(mon->m_size.x - w * k) / 2, (mon->m_size.y - h * k) / 2, w * k, h * k, k};
}

// ---------- API de render, por era ----------

Tex makeTexture(cairo_surface_t* s) {
#ifdef HYPRTAB_NEW_RENDER
    return g_pHyprRenderer->createTexture(s);
#else
    return makeShared<CTexture>(DRM_FORMAT_ARGB8888, cairo_image_surface_get_data(s), cairo_image_surface_get_stride(s),
                                Vector2D{(double)cairo_image_surface_get_width(s), (double)cairo_image_surface_get_height(s)});
#endif
}

void fillRect(const CBox& b, const CHyprColor& c, int round, bool blur = false) {
#if defined(HYPRTAB_RENDER_CTX)
    HYPRTAB_GL->renderRect(*g_ctx, b, c, {.round = round, .blur = blur});
#elif defined(HYPRTAB_NEW_RENDER)
    HYPRTAB_GL->renderRect(b, c, {.round = round, .blur = blur});
#else
    HYPRTAB_GL->renderRect(b, c, {.round = round, .blur = blur});
#endif
}

void drawTexture(const Tex& t, const CBox& b) {
#if defined(HYPRTAB_RENDER_CTX)
    HYPRTAB_GL->renderTexture(*g_ctx, t, b, {.a = 1.F});
#elif defined(HYPRTAB_NEW_RENDER)
    HYPRTAB_GL->renderTexture(t, b, {.a = 1.F});
#else
    HYPRTAB_GL->renderTexture(t, b, {.a = 1.F});
#endif
}

PHLMONITOR currentMonitor() {
#ifdef HYPRTAB_FOCUS_STATE
    return Desktop::focusState()->monitor();
#else
    return g_pCompositor->getMonitorFromCursor();
#endif
}

void damage() {
    if (auto m = g_mon.lock())
        g_pHyprRenderer->damageMonitor(m);
}

// ---------- texturas ----------

Tex iconTexture(const std::string& cls, int px) {
    const auto key = cls + "@" + std::to_string(px);
    auto&      c   = iconCache();
    if (auto it = c.find(key); it != c.end())
        return it->second;
    cairo_surface_t* s = icons::load(cls, px);
    Tex              t = makeTexture(s);
    cairo_surface_destroy(s);
    return c[key] = t;
}

Tex labelTexture(const std::string& text, int w, int h) {
    auto& l = labelCache();
    if (l.tex && l.text == text && l.w == w && l.h == h)
        return l.tex;

    cairo_surface_t* s      = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t*         cr     = cairo_create(s);
    auto*            layout = pango_cairo_create_layout(cr);
    auto*            fd     = pango_font_description_from_string(("Sans " + std::to_string(std::max(8, h * 4 / 9))).c_str());
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_width(layout, w * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_text(layout, text.c_str(), -1);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(fd);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_flush(s);

    l.text = text;
    l.w    = w;
    l.h    = h;
    l.tex  = makeTexture(s);
    cairo_surface_destroy(s);
    return l.tex;
}

// box blur separável (3 passadas ~ gaussiana) só no canal alfa; a sombra é preta, então
// o ARGB pré-multiplicado se resume ao alfa.
void boxBlurAlpha(std::vector<float>& a, int w, int h, int r) {
    if (r < 1)
        return;
    std::vector<float> tmp(a.size());
    auto pass = [&](bool horiz) {
        const int  n = horiz ? w : h, lines = horiz ? h : w;
        const float inv = 1.F / (2 * r + 1);
        for (int l = 0; l < lines; l++) {
            auto at = [&](int i) -> float {
                i = std::clamp(i, 0, n - 1);
                return horiz ? a[(size_t)l * w + i] : a[(size_t)i * w + l];
            };
            float acc = 0;
            for (int i = -r; i <= r; i++)
                acc += at(i);
            for (int i = 0; i < n; i++) {
                (horiz ? tmp[(size_t)l * w + i] : tmp[(size_t)i * w + l]) = acc * inv;
                acc += at(i + r + 1) - at(i - r);
            }
        }
        a.swap(tmp);
    };
    for (int k = 0; k < 3; k++) {
        pass(true);
        pass(false);
    }
}

// textura da sombra: retângulo arredondado preto borrado, com margem de `range` px em volta.
Tex shadowTexture(int bw, int bh, int round, int range) {
    auto& c = shadowCache();
    if (c.tex && c.w == bw && c.h == bh && c.round == round && c.range == range)
        return c.tex;

    const int W = bw + 2 * range, H = bh + 2 * range;
    cairo_surface_t* m  = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
    cairo_t*         cr = cairo_create(m);
    const double     r  = std::min<double>(round, std::min(bw, bh) / 2.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, range + bw - r, range + r, r, -M_PI / 2, 0);
    cairo_arc(cr, range + bw - r, range + bh - r, r, 0, M_PI / 2);
    cairo_arc(cr, range + r, range + bh - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, range + r, range + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_flush(m);

    std::vector<float> a((size_t)W * H);
    const int          ms = cairo_image_surface_get_stride(m);
    const auto*        md = cairo_image_surface_get_data(m);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            a[(size_t)y * W + x] = md[(size_t)y * ms + x] / 255.F;
    cairo_surface_destroy(m);

    boxBlurAlpha(a, W, H, std::max(1, range / 5));

    cairo_surface_t* s    = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    const int        ss   = cairo_image_surface_get_stride(s);
    auto*            sd   = cairo_image_surface_get_data(s);
    for (int y = 0; y < H; y++) {
        auto* row = reinterpret_cast<uint32_t*>(sd + (size_t)y * ss);
        for (int x = 0; x < W; x++)
            row[x] = (uint32_t)std::lround(std::clamp((double)a[(size_t)y * W + x] * SHADOW_A, 0.0, 1.0) * 255.0) << 24;
    }
    cairo_surface_mark_dirty(s);

    c.w = bw; c.h = bh; c.round = round; c.range = range;
    c.tex = makeTexture(s);
    cairo_surface_destroy(s);
    return c.tex;
}

// ---------- desenho ----------

void draw(const PHLMONITOR& mon) {
    if (!g_visible || g_items.empty() || mon != g_mon.lock())
        return;

    const double sc = mon->m_scale;
    const auto   g  = geometry(mon);
    auto         px = [&](double v) { return v * sc; };

    {
        const int range = std::max(4, (int)std::lround(px(SHADOW_RANGE * g.k)));
        const int bw    = (int)std::lround(px(g.w)), bh = (int)std::lround(px(g.h));
        drawTexture(shadowTexture(bw, bh, (int)px(RADIUS * g.k), range),
                    CBox{px(g.x) - range, px(g.y + SHADOW_DY * g.k) - range, (double)bw + 2 * range, (double)bh + 2 * range});
    }

    fillRect(CBox{px(g.x), px(g.y), px(g.w), px(g.h)}, CHyprColor{0.08, 0.08, 0.10, BAR_ALPHA}, (int)px(RADIUS * g.k), true);

    const double cell = (ICON + 2 * PAD) * g.k;
    for (size_t i = 0; i < g_items.size(); i++) {
        const double cx = g.x + BARPAD * g.k + (double)i * (cell + GAP * g.k);
        const double cy = g.y + BARPAD * g.k;

        if (i == g_sel)
            fillRect(CBox{px(cx), px(cy), px(cell), px(cell)}, CHyprColor{1, 1, 1, 0.22}, (int)px(SEL_RADIUS * g.k));

        const int iconPx = std::max(16, (int)std::lround(px(ICON * g.k)));
        drawTexture(iconTexture(g_items[i].cls, iconPx), CBox{px(cx + PAD * g.k), px(cy + PAD * g.k), (double)iconPx, (double)iconPx});
    }

    const int    lw = std::max(32, (int)std::lround(px(g.w - 2 * BARPAD * g.k)));
    const int    lh = std::max(12, (int)std::lround(px(TITLE_H * g.k)));
    const auto&  it = g_items[std::min(g_sel, g_items.size() - 1)];
    const auto   tx = labelTexture(it.title.empty() ? it.cls : it.title, lw, lh);
    drawTexture(tx, CBox{px(g.x + BARPAD * g.k), px(g.y + g.h - (BARPAD * g.k) - TITLE_H * g.k), (double)lw, (double)lh});
}

// ---------- registro do hook de render, por era ----------

#ifdef HYPRTAB_EVENT_BUS
CHyprSignalListener g_renderListener;
#else
SP<HOOK_CALLBACK_FN> g_renderCb;
#endif

void registerRender() {
#if defined(HYPRTAB_RENDER_CTX)
    // 0.57+: o evento traz monitor e contexto
    g_renderListener = Event::bus()->m_events.render.stage.listen([](Event::SRenderStageEvent ev) {
        if (ev.stage != RENDER_LAST_MOMENT || !ev.context || !ev.monitor)
            return;
        g_ctx = &ev.context->get();
        draw(ev.monitor);
        g_ctx = nullptr;
    });
#elif defined(HYPRTAB_EVENT_BUS)
    // 0.54 - 0.56: o evento só traz o estágio; o monitor está em m_renderData
    g_renderListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage != RENDER_LAST_MOMENT)
            return;
        auto mon = HYPRTAB_GL->m_renderData.pMonitor.lock();
        if (mon)
            draw(mon);
    });
#else
    // <= 0.53: hook por nome
    g_renderCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "render", [](void*, SCallbackInfo&, std::any data) {
        try {
            if (std::any_cast<eRenderStage>(data) != RENDER_LAST_MOMENT)
                return;
        } catch (...) {
            return;
        }
        if (auto mon = HYPRTAB_GL->m_renderData.pMonitor.lock())
            draw(mon);
    });
#endif
}

} // namespace

namespace ui {

void init() {
    registerRender();
}

void shutdown() {
#ifdef HYPRTAB_EVENT_BUS
    g_renderListener.reset();
#else
    g_renderCb.reset();
#endif
    g_visible = false;
}

void show(std::vector<Item> items, size_t selected) {
    g_items   = std::move(items);
    g_sel     = selected;
    g_visible = true;
    g_mon     = currentMonitor();
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

    const auto   g  = geometry(mon);
    const double lx = global.x - mon->m_position.x;
    const double ly = global.y - mon->m_position.y;
    if (lx < g.x || lx > g.x + g.w || ly < g.y || ly > g.y + g.h)
        return -2;

    const double cell = (ICON + 2 * PAD) * g.k;
    for (size_t i = 0; i < g_items.size(); i++) {
        const double cx = g.x + BARPAD * g.k + (double)i * (cell + GAP * g.k);
        const double cy = g.y + BARPAD * g.k;
        if (lx >= cx && lx <= cx + cell && ly >= cy && ly <= cy + cell)
            return (int)i;
    }
    return -1;
}

} // namespace ui
