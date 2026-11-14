#include "chrome.hpp"

#include <algorithm>
#include <cmath>

namespace pager {

namespace {

struct RGB {
    double r, g, b;
};
// cromo com reflexo azul-esverdeado (teal); luz vindo de cima à esquerda. WARM: a luz do refletor,
// TEAL: o aro da lente
constexpr RGB DARK{0.03, 0.07, 0.07}, MID{0.40, 0.52, 0.51}, LIGHT{0.90, 0.96, 0.94}, WARM{0.88, 1.0, 0.96}, TEAL{0.25, 0.80, 0.82};

RGB mix(RGB a, RGB b, double t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
void src(cairo_t* cr, RGB c, double a = 1) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
}

void squircle(cairo_t* cr, double x, double y, double w, double h, double r) {
    r = std::min({r, w / 2, h / 2});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

// sombra suave do caminho atual sobre o fundo (deslocada para baixo/direita): traços cada vez mais
// largos e transparentes fazem o papel do blur
void shadowStroke(cairo_t* cr, double w, double off) {
    cairo_save(cr);
    cairo_translate(cr, off, off * 1.3);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (int k = 4; k >= 0; k--) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.10);
        cairo_set_line_width(cr, w + k * off * 0.9);
        cairo_stroke_preserve(cr);
    }
    cairo_restore(cr);
}

// cano cilíndrico no caminho atual: camadas concêntricas do escuro (bordas) ao claro (centro), e um
// reflexo especular fino deslocado para a luz
void tube(cairo_t* cr, double w) {
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    shadowStroke(cr, w, w * 0.45);
    constexpr int N = 7;
    for (int i = 0; i < N; i++) {
        const double t = (double)i / (N - 1); // 0 = borda, 1 = centro
        src(cr, i == 0 ? DARK : mix(mix(DARK, MID, 0.6), mix(MID, LIGHT, 0.55), std::pow(t, 1.4)));
        cairo_set_line_width(cr, w * (1 - 0.8 * t));
        cairo_stroke_preserve(cr);
    }
    cairo_save(cr);
    cairo_translate(cr, -w * 0.14, -w * 0.16);
    src(cr, WARM, 0.85);
    cairo_set_line_width(cr, w * 0.14);
    cairo_stroke(cr);
    cairo_restore(cr);
    cairo_new_path(cr);
}

double radius(const Layout& l) {
    return l.screenW * RADIUS;
}

// caixa de TV em volta do vidro: anel de metal com bisel (claro em cima/esquerda, escuro embaixo/direita)
void casing(cairo_t* cr, const Layout& l, const Box& s) {
    const double r = radius(l), b = l.tube * 1.15; // espessura do anel
    const double x = s.x - b, y = s.y - b, w = s.w + 2 * b, h = s.h + 2 * b, rr = r + b;

    // sombra da caixa no fundo
    squircle(cr, x, y, w, h, rr);
    shadowStroke(cr, b * 0.6, l.tube * 0.6);
    cairo_new_path(cr);

    // corpo: degradê diagonal
    squircle(cr, x, y, w, h, rr);
    auto* g = cairo_pattern_create_linear(x, y, x + w * 0.5, y + h);
    cairo_pattern_add_color_stop_rgb(g, 0, LIGHT.r, LIGHT.g, LIGHT.b);
    cairo_pattern_add_color_stop_rgb(g, 0.35, MID.r, MID.g, MID.b);
    cairo_pattern_add_color_stop_rgb(g, 1, DARK.r * 1.6, DARK.g * 1.6, DARK.b * 1.6);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    src(cr, DARK);
    cairo_set_line_width(cr, std::max(1.0, l.tube * 0.25));
    cairo_stroke(cr);

    // bisel interno: fio escuro em cima/esquerda (a borda do vidro afunda) e claro embaixo/direita
    squircle(cr, s.x - b * 0.25, s.y - b * 0.25, s.w + b * 0.5, s.h + b * 0.5, r + b * 0.25);
    auto* bev = cairo_pattern_create_linear(s.x, s.y, s.x + s.w * 0.4, s.y + s.h);
    cairo_pattern_add_color_stop_rgba(bev, 0, 0, 0, 0, 0.9);
    cairo_pattern_add_color_stop_rgba(bev, 1, LIGHT.r, LIGHT.g, LIGHT.b, 0.7);
    cairo_set_source(cr, bev);
    cairo_set_line_width(cr, b * 0.5);
    cairo_stroke(cr);
    cairo_pattern_destroy(bev);

    // engate à esquerda, no meio
    const double ex = x - l.tube * 0.9, ey = s.y + s.h / 2 - l.tube * 0.9;
    cairo_rectangle(cr, ex, ey, l.tube * 1.2, l.tube * 1.8);
    auto* eg = cairo_pattern_create_linear(0, ey, 0, ey + l.tube * 1.8);
    cairo_pattern_add_color_stop_rgb(eg, 0, LIGHT.r, LIGHT.g, LIGHT.b);
    cairo_pattern_add_color_stop_rgb(eg, 1, DARK.r * 2, DARK.g * 2, DARK.b * 2);
    cairo_set_source(cr, eg);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(eg);
    src(cr, DARK);
    cairo_set_line_width(cr, std::max(1.0, l.tube * 0.2));
    cairo_stroke(cr);
}

// vidro de tubo de imagem: preto, abaulado (centro um pouco mais claro), com sombra interna em cima/esquerda
void glass(cairo_t* cr, const Layout& l, const Box& s) {
    const double r = radius(l);
    squircle(cr, s.x, s.y, s.w, s.h, r);
    auto* g = cairo_pattern_create_radial(s.x + s.w * 0.55, s.y + s.h * 0.55, 0, s.x + s.w * 0.5, s.y + s.h * 0.5, s.w * 0.7);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.05, 0.07, 0.07);
    cairo_pattern_add_color_stop_rgb(g, 1, 0, 0, 0);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    cairo_save(cr);
    squircle(cr, s.x, s.y, s.w, s.h, r);
    cairo_clip(cr);
    for (int k = 0; k < 4; k++) { // sombra interna
        squircle(cr, s.x + l.tube * 0.6, s.y + l.tube * 0.6, s.w, s.h, r);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
        cairo_set_line_width(cr, l.tube * (0.6 + k * 0.5));
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

// pivô do refletor: esfera de latão no fim do cano
void pivot(cairo_t* cr, double cx, double cy, double r) {
    cairo_arc(cr, cx + r * 0.3, cy + r * 0.4, r * 1.1, 0, 2 * M_PI); // sombra
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_fill(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    auto* k = cairo_pattern_create_radial(cx - r * 0.35, cy - r * 0.35, r * 0.05, cx, cy, r);
    cairo_pattern_add_color_stop_rgb(k, 0, WARM.r, WARM.g, WARM.b);
    cairo_pattern_add_color_stop_rgb(k, 0.35, MID.r * 1.3, MID.g * 1.3, MID.b * 1.3);
    cairo_pattern_add_color_stop_rgb(k, 1, DARK.r, DARK.g, DARK.b);
    cairo_set_source(cr, k);
    cairo_fill(cr);
}

cairo_surface_t* surface(double w, double h, double scale, cairo_t*& cr) {
    auto* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, std::max(1, (int)std::ceil(w * scale)), std::max(1, (int)std::ceil(h * scale)));
    cr      = cairo_create(s);
    cairo_scale(cr, scale, scale);
    return s;
}

} // namespace

// ---------- refletor ----------

double lampLit(double t) {
    return std::clamp((t - 0.55) / 0.45, 0.0, 1.0); // só acende chegando na tela
}

double lampAngle(const Layout& l, size_t i, double t) {
    const auto&  s   = l.screens[i];
    const auto&  b   = l.brackets[i];
    const double px  = b.x + b.w / 2, py = b.y + b.h / 2;
    const double on  = std::atan2(s.y + s.h * 0.5 - py, s.x + s.w * 0.45 - px); // para o meio da tela (à esquerda)
    const double off = M_PI / 2;                                                // pendurado, apontando para baixo
    // gira por baixo, para a esquerda (de 90° até ~170°, sem dar a volta por cima)
    const double to = on < 0 ? on + 2 * M_PI : on;
    const double e  = t * t * (3 - 2 * t); // suave nas pontas
    return off + (to - off) * e;
}

Box lampCell(const Layout& l, size_t i) {
    // célula do refletor: da borda esquerda do vidro até depois da placa, do cano de cima até a base da tela
    const auto&  s = l.screens[i];
    const double y = s.y - l.screenW * GAP * 0.75;
    return {s.x - l.tube * 2, y, l.w - (s.x - l.tube * 2), s.y + s.h + l.tube * 2 - y};
}

cairo_surface_t* drawLamp(const Layout& l, double scale, double t) {
    const Box    c  = lampCell(l, 0);
    cairo_t*     cr = nullptr;
    auto*        sf = surface(c.w, c.h, scale, cr);
    cairo_translate(cr, -c.x, -c.y);
    const auto&  b   = l.brackets[0];
    const double px  = b.x + b.w / 2, py = b.y + b.h / 2;
    // refletor curto e "gordinho": traseira em cúpula em volta do pivô, corpo que alarga até a boca
    const double L   = b.w * 1.0;   // do pivô até a boca
    const double HB  = L * 0.34;    // meia altura na traseira
    const double HF  = L * 0.48;    // meia altura na boca
    const double F   = L * 0.72;    // posição da boca ao longo do eixo
    const double a   = lampAngle(l, 0, t);
    const double lit = lampLit(t);

    // contorno do corpo, no sistema do refletor (eixo x = para onde aponta)
    auto bodyPath = [&] {
        cairo_new_path(cr);
        cairo_arc(cr, 0, 0, HB, M_PI / 2, 3 * M_PI / 2);                  // cúpula de trás
        cairo_curve_to(cr, F * 0.45, -HB, F * 0.75, -HF * 0.95, F, -HF);   // ombro de cima
        cairo_line_to(cr, F, HF);
        cairo_curve_to(cr, F * 0.75, HF * 0.95, F * 0.45, HB, 0, HB);      // ombro de baixo
        cairo_close_path(cr);
    };

    // facho: trapézio da boca até a tela, com degradê ao longo do eixo (por baixo do corpo)
    if (lit > 0) {
        cairo_save(cr);
        cairo_translate(cr, px, py);
        cairo_rotate(cr, a);
        const double x0 = F, x1 = std::hypot(l.screens[0].x + l.screens[0].w * 0.45 - px, l.screens[0].y + l.screens[0].h * 0.5 - py) + l.screenW * 0.25;
        cairo_move_to(cr, x0, -HF * 0.85);
        cairo_line_to(cr, x1, -l.screenH * 0.45);
        cairo_line_to(cr, x1, l.screenH * 0.45);
        cairo_line_to(cr, x0, HF * 0.85);
        cairo_close_path(cr);
        auto* g = cairo_pattern_create_linear(x0, 0, x1, 0);
        cairo_pattern_add_color_stop_rgba(g, 0, WARM.r, WARM.g, WARM.b, 0.6 * lit);
        cairo_pattern_add_color_stop_rgba(g, 1, WARM.r, WARM.g, WARM.b, 0.0);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        cairo_restore(cr);
    }

    cairo_save(cr);
    cairo_translate(cr, px, py);
    // sombra do refletor no fundo
    cairo_save(cr);
    cairo_translate(cr, L * 0.1, L * 0.14);
    cairo_rotate(cr, a);
    bodyPath();
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_fill(cr);
    cairo_restore(cr);

    cairo_rotate(cr, a);
    // corpo, sombreado como cilindro (claro do lado de cima do eixo)
    bodyPath();
    auto* body = cairo_pattern_create_linear(0, -HF, 0, HF);
    cairo_pattern_add_color_stop_rgb(body, 0, DARK.r * 2, DARK.g * 2, DARK.b * 2);
    cairo_pattern_add_color_stop_rgb(body, 0.22, LIGHT.r, LIGHT.g, LIGHT.b);
    cairo_pattern_add_color_stop_rgb(body, 0.45, MID.r, MID.g, MID.b);
    cairo_pattern_add_color_stop_rgb(body, 1, DARK.r, DARK.g, DARK.b);
    cairo_set_source(cr, body);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(body);
    src(cr, DARK);
    cairo_set_line_width(cr, std::max(0.8, l.tube * 0.2));
    cairo_stroke(cr);
    // aro e lente na boca (elipse: o círculo visto de lado)
    cairo_save(cr);
    cairo_translate(cr, F, 0);
    cairo_scale(cr, L * 0.14, HF);
    cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
    cairo_restore(cr);
    src(cr, TEAL);
    cairo_fill(cr);
    cairo_save(cr);
    cairo_translate(cr, F + L * 0.01, 0);
    cairo_scale(cr, L * 0.10, HF * 0.8);
    cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
    cairo_restore(cr);
    src(cr, mix({0.06, 0.20, 0.21}, WARM, lit));
    cairo_fill(cr);
    if (lit > 0) { // brilho da lente acesa
        auto* glow = cairo_pattern_create_radial(F + L * 0.03, 0, 0, F + L * 0.03, 0, L * 0.6);
        cairo_pattern_add_color_stop_rgba(glow, 0, WARM.r, WARM.g, WARM.b, 0.8 * lit);
        cairo_pattern_add_color_stop_rgba(glow, 1, WARM.r, WARM.g, WARM.b, 0);
        cairo_arc(cr, F + L * 0.03, 0, L * 0.6, 0, 2 * M_PI);
        cairo_set_source(cr, glow);
        cairo_fill(cr);
        cairo_pattern_destroy(glow);
    }
    cairo_restore(cr);

    // pivô por cima (o refletor gira em volta dele)
    pivot(cr, px, py, l.tube * 1.05);

    cairo_destroy(cr);
    cairo_surface_flush(sf);
    return sf;
}

cairo_surface_t* drawLightPool(const Layout& l, double scale) {
    const Box    c  = lampCell(l, 0);
    cairo_t*     cr = nullptr;
    auto*        sf = surface(c.w, c.h, scale, cr);
    cairo_translate(cr, -c.x, -c.y);
    const auto&  s = l.screens[0];
    squircle(cr, s.x, s.y, s.w, s.h, radius(l));
    cairo_clip(cr);
    const double cx = s.x + s.w * 0.55, cy = s.y + s.h * 0.45;
    auto*        g  = cairo_pattern_create_radial(cx, cy, 0, cx, cy, s.w * 0.6);
    cairo_pattern_add_color_stop_rgba(g, 0, WARM.r, WARM.g, WARM.b, 0.30);
    cairo_pattern_add_color_stop_rgba(g, 0.6, WARM.r, WARM.g, WARM.b, 0.10);
    cairo_pattern_add_color_stop_rgba(g, 1, WARM.r, WARM.g, WARM.b, 0.0);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);
    cairo_destroy(cr);
    cairo_surface_flush(sf);
    return sf;
}

// ---------- base e brilho ----------

cairo_surface_t* drawBase(const Layout& l, double scale) {
    cairo_t*     cr = nullptr;
    auto*        s  = surface(l.w, l.h, scale, cr);
    const double tw = l.tube, gap = l.screenW * GAP;
    for (size_t i = 0; i < l.screens.size(); i++) {
        const auto&  sc = l.screens[i];
        const auto&  b  = l.brackets[i];
        const double px = sc.x + sc.w * 0.45, up = sc.y - gap * 0.55;
        // cano vertical ligando à tela de baixo
        if (i + 1 < l.screens.size()) {
            cairo_move_to(cr, px, sc.y + sc.h);
            cairo_line_to(cr, px, l.screens[i + 1].y);
            tube(cr, tw);
        }
        // cano: sobe do topo da tela, vai para a direita e desce na diagonal até o pivô do refletor
        cairo_move_to(cr, px, sc.y);
        cairo_line_to(cr, px, up + tw);
        cairo_curve_to(cr, px, up, px, up, px + tw, up);
        cairo_line_to(cr, sc.x + sc.w - tw * 0.5, up);
        cairo_line_to(cr, b.x + b.w / 2, b.y + b.h / 2);
        tube(cr, tw);
        casing(cr, l, sc);
        glass(cr, l, sc);
    }
    cairo_destroy(cr);
    cairo_surface_flush(s);
    return s;
}

cairo_surface_t* drawGloss(const Layout& l, double scale) {
    cairo_t*     cr = nullptr;
    auto*        s  = surface(l.w, l.h, scale, cr);
    const double r  = radius(l);
    for (const auto& sc : l.screens) {
        cairo_save(cr);
        squircle(cr, sc.x, sc.y, sc.w, sc.h, r);
        cairo_clip(cr);
        // reflexo da janela da sala: faixa curva clara no alto do vidro
        cairo_move_to(cr, sc.x + sc.w * 0.1, sc.y + sc.h * 0.30);
        cairo_curve_to(cr, sc.x + sc.w * 0.25, sc.y + sc.h * 0.08, sc.x + sc.w * 0.75, sc.y + sc.h * 0.08, sc.x + sc.w * 0.9, sc.y + sc.h * 0.30);
        cairo_curve_to(cr, sc.x + sc.w * 0.75, sc.y + sc.h * 0.16, sc.x + sc.w * 0.25, sc.y + sc.h * 0.16, sc.x + sc.w * 0.1, sc.y + sc.h * 0.30);
        cairo_close_path(cr);
        auto* refl = cairo_pattern_create_linear(0, sc.y + sc.h * 0.08, 0, sc.y + sc.h * 0.3);
        cairo_pattern_add_color_stop_rgba(refl, 0, 1, 1, 1, 0.75);
        cairo_pattern_add_color_stop_rgba(refl, 1, 1, 1, 1, 0.05);
        cairo_set_source(cr, refl);
        cairo_fill(cr);
        cairo_pattern_destroy(refl);
        // brilho azul pequeno no canto de baixo à direita
        const double gx = sc.x + sc.w - r * 0.45, gy = sc.y + sc.h - r * 0.45;
        auto*        glint = cairo_pattern_create_radial(gx, gy, 0, gx, gy, r * 0.5);
        cairo_pattern_add_color_stop_rgba(glint, 0, 0.35, 0.75, 0.95, 0.55);
        cairo_pattern_add_color_stop_rgba(glint, 1, 0.35, 0.75, 0.95, 0);
        cairo_set_source(cr, glint);
        cairo_paint(cr);
        cairo_pattern_destroy(glint);
        cairo_restore(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(s);
    return s;
}

} // namespace pager
