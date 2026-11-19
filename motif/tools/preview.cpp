// Pré-visualização da moldura: usa o mesmo layout() e paintFrame() do plugin, mas desenha com cairo.
// NÃO é uma captura do Hyprland (sem GL, sem fonte/escala do compositor); serve para ajustar proporções
// e cores. Uso: preview saida.png [border title scale]
#include "../src/paint.hpp"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <cmath>
#include <cstdlib>
#include <string>

using namespace motif;

namespace {

struct CairoCanvas : Canvas {
    cairo_t* cr;
    double   ox, oy, s;
    void     fill(const Rect& r, const Color& c) override {
        // arestas arredondadas, como o plugin faz, para peças vizinhas se encostarem
        const double x0 = std::round((ox + r.x) * s), y0 = std::round((oy + r.y) * s);
        const double x1 = std::round((ox + r.x + r.w) * s), y1 = std::round((oy + r.y + r.h) * s);
        if (x1 - x0 < 1 || y1 - y0 < 1)
            return;
        cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
        cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
        cairo_fill(cr);
    }
};

Color rgb(unsigned v) {
    return {((v >> 16) & 255) / 255.F, ((v >> 8) & 255) / 255.F, (v & 255) / 255.F, 1};
}

struct Win {
    double      x, y, w, h;
    bool        active;
    std::string title;
    std::string body;
    Part        pressed = Part::None;
    bool        shaded  = false;
};

void drawWin(cairo_t* cr, const Win& wn, int B, int T, double S) {
    const Layout L = layout(wn.w, wn.h, B, T);

    // cliente (terminal escuro)
    {
        const double cx = (wn.x + L.client.x) * S, cy = (wn.y + L.client.y) * S;
        cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
        cairo_rectangle(cr, cx, cy, L.client.w * S, L.client.h * S);
        cairo_fill(cr);
        auto* lay = pango_cairo_create_layout(cr);
        auto* fd  = pango_font_description_from_string("Monospace");
        pango_font_description_set_size(fd, 11 * S * PANGO_SCALE);
        pango_layout_set_font_description(lay, fd);
        pango_layout_set_text(lay, wn.body.c_str(), -1);
        cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
        cairo_save(cr);
        cairo_rectangle(cr, cx, cy, L.client.w * S, L.client.h * S);
        cairo_clip(cr);
        cairo_move_to(cr, cx + 4 * S, cy + 3 * S); // depois do clip, que apaga o ponto atual
        pango_cairo_show_layout(cr, lay);
        cairo_restore(cr);
        pango_font_description_free(fd);
        g_object_unref(lay);
    }

    // cores padrão do plugin
    const Palette pal = makePalette(wn.active ? rgb(0xaeb2c3) : rgb(0xbcbcbc), std::nullopt, std::nullopt);
    CairoCanvas   cv;
    cv.cr = cr, cv.ox = wn.x, cv.oy = wn.y, cv.s = S;
    paintFrame(cv, L, wn.w, wn.h, B, T, pal, {wn.pressed, wn.shaded});

    // título centralizado entre os botões (Sans Bold, como o padrão do plugin)
    if (L.text.w > 4) {
        auto* lay = pango_cairo_create_layout(cr);
        auto* fd  = pango_font_description_from_string("Sans Bold");
        pango_font_description_set_size(fd, 10 * S * PANGO_SCALE);
        pango_layout_set_font_description(lay, fd);
        pango_layout_set_width(lay, (int)(L.text.w * S) * PANGO_SCALE);
        pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
        pango_layout_set_alignment(lay, PANGO_ALIGN_CENTER);
        pango_layout_set_text(lay, wn.title.c_str(), -1);
        int lw, lh;
        pango_layout_get_size(lay, &lw, &lh);
        if (wn.active)
            cairo_set_source_rgb(cr, 0, 0, 0);
        else
            cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
        cairo_move_to(cr, std::round((wn.x + L.text.x) * S), std::round((wn.y + L.text.y) * S + (L.text.h * S - lh / (double)PANGO_SCALE) / 2));
        pango_cairo_show_layout(cr, lay);
        pango_font_description_free(fd);
        g_object_unref(lay);
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "preview.png";
    const int   B   = argc > 2 ? std::atoi(argv[2]) : 4;
    const int   T   = argc > 3 ? std::atoi(argv[3]) : 18;
    const double S  = argc > 4 ? std::atof(argv[4]) : 1.0;

    const int W = 800, H = 450;
    auto*     surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W * S, H * S);
    auto*     cr   = cairo_create(surf);
    cairo_set_source_rgb(cr, 0.478, 0.549, 0.627); // fundo azul acinzentado da imagem de referência
    cairo_paint(cr);

    const Win wins[] = {
        {30, 20, 330, 130, false, "xterm", "motif$ toilet -f slant \"motif wm!\""},
        {250, 60, 300, 130, true, "xterm", "motif$ ufetch\n  OS:  OpenBSD\n  WM:  mwm", Part::None},
        {60, 230, 420, 150, false, "xterm", "motif$ ls\nCVS  Makefile  README  archivers"},
        {520, 250, 220, 90, true, "um título bem comprido que não cabe na barra", "motif$ scrot -d 5", Part::Maximize},
        {560, 20, 200, 60, false, "xload", "", Part::None, true},
    };
    for (auto& w : wins)
        drawWin(cr, w, B, T, S);

    cairo_surface_write_to_png(surf, out);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return 0;
}
