#pragma once

#include "model.hpp"

#include <cairo/cairo.h>

// Desenho do pager em cairo (sem dependência do Hyprland), com volume: luz vindo de cima à esquerda,
// canos cilíndricos de latão, caixas de TV com bisel, vidro afundado e abaulado, sombras no fundo.
// Feito do zero, no estilo dos temas metálicos clássicos do Enlightenment.
//
// Camadas (px físicos = medidas * scale; o chamador libera com cairo_surface_destroy):
//   base       canos, caixas e vidros (o pager inteiro), por baixo das janelas
//   luz        o círculo de luz no vidro da tela iluminada (célula de uma tela), desenhado com alfa
//   brilho     reflexos no vidro (o pager inteiro), por cima das janelas
//   refletor   o refletor da tela no ângulo do momento, com o facho e o pivô (célula de uma tela)
//
// Refletor: parado ele aponta para cima, apagado; `t` = 1 é virado para a tela, aceso. Entre os dois ele
// gira (com aceleração suave) e só acende perto do fim. As células são iguais para todas as telas
// (só mudam de lugar): `lampCell(l, i)` diz onde desenhar a de cada uma.
namespace pager {

cairo_surface_t* drawBase(const Layout& l, double scale);
cairo_surface_t* drawGloss(const Layout& l, double scale);
cairo_surface_t* drawLamp(const Layout& l, double scale, double t);
cairo_surface_t* drawLightPool(const Layout& l, double scale);

Box    lampCell(const Layout& l, size_t i);
double lampAngle(const Layout& l, size_t i, double t); // radianos
double lampLit(double t);                               // quanto está aceso (0..1) no ponto `t` da rotação

} // namespace pager
