#pragma once
#include <cairo/cairo.h>
#include <string>

// Resolve a classe da janela em um ícone e devolve uma surface ARGB32 px x px
// (premultiplicada). Nunca devolve nulo: sem ícone, desenha um círculo com a inicial.
// O chamador libera com cairo_surface_destroy.
namespace icons {
cairo_surface_t* load(const std::string& windowClass, int px);

// dados do .desktop que corresponde à classe (StartupWMClass, nome do arquivo ou sufixo
// depois do último ponto). nullptr se não houver. `exec` vem cru, com os códigos %U/%f etc.
struct DesktopEntry {
    std::string icon, exec, name;
};
const DesktopEntry* desktopEntry(const std::string& windowClass);
}
