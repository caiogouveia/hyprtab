#pragma once
#include <cairo/cairo.h>
#include <string>

// Resolve a classe da janela em um ícone e devolve uma surface ARGB32 px x px
// (premultiplicada). Nunca devolve nulo: sem ícone, desenha um círculo com a inicial.
// O chamador libera com cairo_surface_destroy.
namespace icons {
cairo_surface_t* load(const std::string& windowClass, int px);
}
