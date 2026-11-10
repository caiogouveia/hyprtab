// Desenho compartilhado: texturas, retângulos, painel de vidro e hook de render, por era do Hyprland.

#include "render.hpp"
#include "icons.hpp"

#include <drm_fourcc.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <unordered_map>
#include <vector>

#ifndef HYPRTAB_NEW_RENDER
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#endif

namespace render {

namespace {

// sombra projetada: alcance do blur e deslocamento vertical; alfa máximo do preto
constexpr double SHADOW_RANGE = 44, SHADOW_DY = 12, SHADOW_A = 0.55;
// fundo do painel: translúcido para o blur do compositor aparecer por trás
constexpr double PANEL_ALPHA = 0.55;
// borda dupla estilo macOS: traço escuro por fora (separa do fundo) e traço claro logo por dentro
// (reflexo do vidro, mais forte no topo e suave nas laterais); um véu claro curto desce do topo
constexpr double RIM_W = 1, OUTER_A = 0.45, INNER_TOP_A = 0.22, INNER_SIDE_A = 0.07, INNER_FADE = 0.35;
constexpr double SHEEN_H = 22, SHEEN_A = 0.05;

#ifdef HYPRTAB_RENDER_CTX
Render::CRenderContext* g_ctx = nullptr; // válido só durante o callback de render
#endif

void (*g_draw)(const PHLMONITOR&) = nullptr;

// caches por chave; nunca liberados (ver render.hpp), só limpos quando crescem demais.
std::unordered_map<std::string, Tex>& cache(int which) {
    static auto* c = new std::unordered_map<std::string, Tex>[5]();
    auto&        m = c[which];
    if (m.size() > 64)
        m.clear();
    return m;
}
enum { ICONS, LABELS, SHADOWS, RIMS, SLICES };

// contorno arredondado recuado `inset` px para dentro do retângulo w×h
void roundedPath(cairo_t* cr, double w, double h, double round, double inset) {
    const double r = std::max(0.0, std::min(round, std::min(w, h) / 2.0) - inset);
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - inset - r, inset + r, r, -M_PI / 2, 0);
    cairo_arc(cr, w - inset - r, h - inset - r, r, 0, M_PI / 2);
    cairo_arc(cr, inset + r, h - inset - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, inset + r, inset + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
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

// sombra: retângulo arredondado preto borrado, com margem de `range` px em volta.
cairo_surface_t* shadowSurface(int bw, int bh, int round, int range) {
    const int W = bw + 2 * range, H = bh + 2 * range;
    cairo_surface_t* m  = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
    cairo_t*         cr = cairo_create(m);
    cairo_translate(cr, range, range);
    roundedPath(cr, bw, bh, round, 0);
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
    return s;
}

// onde a borda clara termina de esmaecer a partir do topo (e começa a esmaecer antes da base), em px.
// Proporcional à altura até a do switcher; painéis mais altos (dock vertical) param aí.
double rimFade(int bh, int round) {
    return std::max((double)round, INNER_FADE * std::min(bh, 194));
}

// borda dupla + véu, do tamanho do painel.
cairo_surface_t* rimSurface(int bw, int bh, int round, int lw, int sheenPx) {
    cairo_surface_t* s  = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
    cairo_t*         cr = cairo_create(s);
    cairo_set_line_width(cr, lw);

    // véu: degradê curto do topo para baixo, recortado pelo formato do painel
    cairo_save(cr);
    roundedPath(cr, bw, bh, round, 0);
    cairo_clip(cr);
    auto* sheen = cairo_pattern_create_linear(0, 0, 0, sheenPx);
    cairo_pattern_add_color_stop_rgba(sheen, 0, 1, 1, 1, SHEEN_A);
    cairo_pattern_add_color_stop_rgba(sheen, 1, 1, 1, 1, 0);
    cairo_set_source(cr, sheen);
    cairo_paint(cr);
    cairo_pattern_destroy(sheen);
    cairo_restore(cr);

    // borda de fora: escura e uniforme, exatamente na beirada do painel
    roundedPath(cr, bw, bh, round, lw / 2.0);
    cairo_set_source_rgba(cr, 0, 0, 0, OUTER_A);
    cairo_stroke(cr);

    // borda de dentro: clara, colada na de fora; forte no topo e some logo depois dos cantos
    roundedPath(cr, bw, bh, round, lw * 1.5);
    // distâncias em px (não frações da altura): entre o fade de cima e o de baixo a borda é
    // uniforme, o que permite fatiar painéis altos na vertical
    const double fade  = rimFade(bh, round);
    auto         at    = [&](double y) { return std::clamp(y / bh, 0.0, 1.0); };
    auto*        inner = cairo_pattern_create_linear(0, 0, 0, bh);
    cairo_pattern_add_color_stop_rgba(inner, 0, 1, 1, 1, INNER_TOP_A);
    cairo_pattern_add_color_stop_rgba(inner, at(round), 1, 1, 1, INNER_TOP_A * 0.6);
    cairo_pattern_add_color_stop_rgba(inner, at(fade), 1, 1, 1, INNER_SIDE_A);
    cairo_pattern_add_color_stop_rgba(inner, std::max(at(fade), at(bh - fade)), 1, 1, 1, INNER_SIDE_A);
    cairo_pattern_add_color_stop_rgba(inner, 1, 1, 1, 1, INNER_SIDE_A * 0.5);
    cairo_set_source(cr, inner);
    cairo_stroke(cr);
    cairo_pattern_destroy(inner);

    cairo_destroy(cr);
    cairo_surface_flush(s);
    return s;
}

// textura inteira com cache por chave (painéis estreitos demais para fatiar)
Tex wholeTexture(int which, const std::string& key, cairo_surface_t* s) {
    auto t = makeTexture(s);
    cairo_surface_destroy(s);
    return cache(which)[key] = t;
}

// ---------- fatias (3-slice) ----------
// Fora dos cantos, sombra e borda são uniformes ao longo do eixo maior do painel: viram
// início + faixa de 1 px (esticada) + fim, geradas uma vez por espessura. Assim um painel que muda
// de comprimento a cada quadro (o zoom da dock, embaixo ou nas laterais) não gera textura nova nem
// refaz o blur na CPU.

struct Slices {
    Tex  start, mid, end;
    int  cap      = 0; // comprimento de cada ponta
    bool vertical = false;
};

Tex crop(cairo_surface_t* src, int x, int y, int w, int h) {
    cairo_surface_t* s  = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t*         cr = cairo_create(s);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, src, -x, -y);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    auto t = makeTexture(s);
    cairo_surface_destroy(s);
    return t;
}

// `make(len)` desenha a textura inteira com comprimento `len` no eixo fatiado; `cap` é o que não é uniforme
template <class F>
const Slices& slices(const std::string& key, int cap, bool vertical, F&& make) {
    static auto* c = new std::unordered_map<std::string, Slices>();
    if (auto it = c->find(key); it != c->end())
        return it->second;
    if (c->size() > 64)
        c->clear();
    const int        len = 2 * cap + 1;
    cairo_surface_t* s   = make(len);
    const int        W = cairo_image_surface_get_width(s), H = cairo_image_surface_get_height(s);
    Slices           sl;
    if (vertical)
        sl = {crop(s, 0, 0, W, cap), crop(s, 0, cap, W, 1), crop(s, 0, H - cap, W, cap), cap, true};
    else
        sl = {crop(s, 0, 0, cap, H), crop(s, cap, 0, 1, H), crop(s, W - cap, 0, cap, H), cap, false};
    cairo_surface_destroy(s);
    return (*c)[key] = sl;
}

void drawSlices(const Slices& sl, double x, double y, double w, double h) {
    if (sl.vertical) {
        drawTexture(sl.start, CBox{x, y, w, (double)sl.cap});
        drawTexture(sl.mid, CBox{x, y + sl.cap, w, h - 2 * sl.cap});
        drawTexture(sl.end, CBox{x, y + h - sl.cap, w, (double)sl.cap});
    } else {
        drawTexture(sl.start, CBox{x, y, (double)sl.cap, h});
        drawTexture(sl.mid, CBox{x + sl.cap, y, w - 2 * sl.cap, h});
        drawTexture(sl.end, CBox{x + w - sl.cap, y, (double)sl.cap, h});
    }
}

#ifdef HYPRTAB_EVENT_BUS
CHyprSignalListener g_renderListener;
#else
SP<HOOK_CALLBACK_FN> g_renderCb;
#endif

} // namespace

// ---------- API de render, por era ----------

Tex makeTexture(cairo_surface_t* s) {
#ifdef HYPRTAB_NEW_RENDER
    return g_pHyprRenderer->createTexture(s);
#else
    return makeShared<CTexture>(DRM_FORMAT_ARGB8888, cairo_image_surface_get_data(s), cairo_image_surface_get_stride(s),
                                Vector2D{(double)cairo_image_surface_get_width(s), (double)cairo_image_surface_get_height(s)});
#endif
}

void fillRect(const CBox& b, const CHyprColor& c, int round, bool blur) {
#if defined(HYPRTAB_RENDER_CTX)
    HYPRTAB_GL->renderRect(*g_ctx, b, c, {.round = round, .blur = blur});
#elif defined(HYPRTAB_NEW_RENDER)
    HYPRTAB_GL->renderRect(b, c, {.round = round, .blur = blur});
#else
    // <= 0.53: a render pass só é desenhada depois do hook; GL direto ficaria por baixo das janelas
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = b, .color = c, .round = round, .blur = blur}));
#endif
}

void drawTexture(const Tex& t, const CBox& b, float alpha) {
#if defined(HYPRTAB_RENDER_CTX)
    HYPRTAB_GL->renderTexture(*g_ctx, t, b, {.a = alpha});
#elif defined(HYPRTAB_NEW_RENDER)
    HYPRTAB_GL->renderTexture(t, b, {.a = alpha});
#else
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(CTexPassElement::SRenderData{.tex = t, .box = b, .a = alpha}));
#endif
}

// ---------- texturas ----------

Tex iconTexture(const std::string& cls, int px) {
    auto&      c   = cache(ICONS);
    const auto key = cls + "@" + std::to_string(px);
    if (auto it = c.find(key); it != c.end())
        return it->second;
    cairo_surface_t* s = icons::load(cls, px);
    Tex              t = makeTexture(s);
    cairo_surface_destroy(s);
    return c[key] = t;
}

Tex labelTexture(const std::string& text, int w, int h, bool center) {
    auto&      c   = cache(LABELS);
    const auto key = std::format("{}x{}{}:{}", w, h, center ? "c" : "l", text);
    if (auto it = c.find(key); it != c.end())
        return it->second;
    if (c.size() > 64) // títulos mudam o tempo todo; não deixa acumular
        c.clear();

    cairo_surface_t* s      = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t*         cr     = cairo_create(s);
    auto*            layout = pango_cairo_create_layout(cr);
    auto*            fd     = pango_font_description_from_string(("Sans " + std::to_string(std::max(8, h * 4 / 9))).c_str());
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_width(layout, w * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, center ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);
    pango_layout_set_text(layout, text.c_str(), -1);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(fd);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_flush(s);

    auto t = makeTexture(s);
    cairo_surface_destroy(s);
    return c[key] = t;
}

// ---------- painel ----------

void glassPanel(const CBox& box, int round, double scale, double k) {
    // px inteiros: as fatias se encostam sem fresta
    const double x = std::round(box.x), y = std::round(box.y);
    const int    bw = (int)std::lround(box.w), bh = (int)std::lround(box.h);
    const bool   vertical = bh > bw; // fatia ao longo do eixo maior (dock nas laterais)

    // sombra: fora dos cantos o blur (alcance < range) já é uniforme
    const int    range = std::max(4, (int)std::lround(SHADOW_RANGE * scale * k));
    const double sy    = std::round(y + SHADOW_DY * scale * k) - range;
    const int    sw = bw + 2 * range, sh = bh + 2 * range;
    const int    scap = 2 * range + round + 1;
    if ((vertical ? sh : sw) >= 2 * scap + 1) {
        const auto key = std::format("s{}{}r{}g{}", vertical ? "v" : "h", vertical ? bw : bh, round, range);
        const auto& sl = slices(key, scap, vertical, [&](int len) {
            return vertical ? shadowSurface(bw, len - 2 * range, round, range) : shadowSurface(len - 2 * range, bh, round, range);
        });
        drawSlices(sl, x - range, sy, sw, sh);
    } else {
        const auto key = std::format("{}x{}r{}g{}", bw, bh, round, range);
        auto&      c   = cache(SHADOWS);
        auto       it  = c.find(key);
        drawTexture(it != c.end() ? it->second : wholeTexture(SHADOWS, key, shadowSurface(bw, bh, round, range)), CBox{x - range, sy, (double)sw, (double)sh});
    }

    fillRect(CBox{x, y, (double)bw, (double)bh}, CHyprColor{0.08, 0.08, 0.10, PANEL_ALPHA}, round, true);

    // borda + véu: uniformes depois da curva dos cantos (na vertical, depois dos fades e do véu)
    const int lw    = std::max(1, (int)std::lround(RIM_W * scale));
    const int sheen = std::max(1, (int)std::lround(SHEEN_H * scale * k));
    const int rcap  = vertical ? (int)std::ceil(std::max({(double)round, rimFade(bh, round), (double)sheen})) + 2 * lw + 1 : round + 2 * lw + 1;
    if ((vertical ? bh : bw) >= 2 * rcap + 1) {
        const auto key = std::format("b{}{}r{}l{}v{}", vertical ? "v" : "h", vertical ? bw : bh, round, lw, sheen);
        const auto& sl = slices(key, rcap, vertical, [&](int len) {
            return vertical ? rimSurface(bw, len, round, lw, sheen) : rimSurface(len, bh, round, lw, sheen);
        });
        drawSlices(sl, x, y, bw, bh);
    } else {
        const auto key = std::format("{}x{}r{}l{}v{}", bw, bh, round, lw, sheen);
        auto&      c   = cache(RIMS);
        auto       it  = c.find(key);
        drawTexture(it != c.end() ? it->second : wholeTexture(RIMS, key, rimSurface(bw, bh, round, lw, sheen)), CBox{x, y, (double)bw, (double)bh});
    }
}

PHLMONITOR currentMonitor() {
#ifdef HYPRTAB_FOCUS_STATE
    return Desktop::focusState()->monitor();
#else
    return g_pCompositor->getMonitorFromCursor();
#endif
}

// ---------- hook de render, por era ----------

void registerRender(void (*draw)(const PHLMONITOR&)) {
    g_draw = draw;
#if defined(HYPRTAB_RENDER_CTX)
    // 0.57+: o evento traz monitor e contexto
    g_renderListener = Event::bus()->m_events.render.stage.listen([](Event::SRenderStageEvent ev) {
        if (ev.stage != RENDER_LAST_MOMENT || !ev.context || !ev.monitor)
            return;
        g_ctx = &ev.context->get();
        g_draw(ev.monitor);
        g_ctx = nullptr;
    });
#elif defined(HYPRTAB_EVENT_BUS)
    // 0.54 - 0.56: o evento só traz o estágio; o monitor está em m_renderData
    g_renderListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage != RENDER_LAST_MOMENT)
            return;
        if (auto mon = HYPRTAB_GL->m_renderData.pMonitor.lock())
            g_draw(mon);
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
            g_draw(mon);
    });
#endif
}

void unregisterRender() {
#ifdef HYPRTAB_EVENT_BUS
    g_renderListener.reset();
#else
    g_renderCb.reset();
#endif
}

} // namespace render
