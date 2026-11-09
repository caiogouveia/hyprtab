#pragma once

#include <cstddef>

// Geometria da barra (sem dependência do Hyprland, testável).
// Tudo em px lógicos, local ao monitor; o render multiplica pela escala.
namespace layout {

constexpr double ICON = 96, PAD = 14, GAP = 6, BARPAD = 18, TITLE_H = 34;

struct Geom {
    double x, y, w, h; // barra
    double k;          // fator de redução quando há muitas janelas
};

struct Cell {
    double x, y, size; // quadrado do ícone i (fundo da seleção)
};

Geom geometry(size_t n, double monW, double monH);
Cell cell(const Geom& g, size_t i);

// >= 0: índice do ícone sob o ponto; -1: dentro da barra mas fora de ícones; -2: fora da barra.
int hit(const Geom& g, size_t n, double lx, double ly);

} // namespace layout
