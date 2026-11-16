#pragma once

#include <array>

// Modelo do hyprborders (sem dependência do Hyprland, testável): medidas da sombra e do contorno.
// Px lógicos, a não ser onde dito.
namespace borders {

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
};

// Uma camada de sombra, nos termos do box-shadow do CSS: `0 offsetY blur spread rgba(0,0,0,alpha)`.
struct Shadow {
    double blur    = 0; // raio do desfoque (σ = blur / 2, como no CSS)
    double offsetY = 0; // descida
    double spread  = 0; // quanto a sombra cresce além da janela antes de desfocar
    double alpha   = 0; // opacidade no miolo
};

// Sombra como a do macOS, em duas camadas:
//   ambiente  grande, difusa e deslocada para baixo (a luz vem de cima)
//   contato   curta e rente à borda: "cola" a janela no fundo
// A janela em foco tem a sombra bem maior; nas outras ela encolhe (não só clareia).
struct Look {
    Shadow ambientActive{70, 22, 4, 0.56};
    Shadow ambientIdle{36, 12, 0, 0.30};
    Shadow contactActive{4, 1, 0, 0.25};
    Shadow contactIdle{3, 1, 0, 0.17};

    // contorno: o mesmo vidro dos outros plugins (hyprtab, dock, menus)
    double outerA     = 0.45; // fio escuro por fora
    double innerTopA  = 0.22; // fio claro por dentro: forte no topo...
    double innerSideA = 0.07; // ...e fraco nas laterais
};

// as camadas na ordem de desenho (ambiente, depois contato), com valores contidos (blur >= 0, alfa em [0, 1])
std::array<Shadow, 2> shadowLayers(const Look& l, bool active);

// Como desenhar uma camada com o shader de sombra do Hyprland. Ele pinta um miolo sólido e esmaece para
// fora com (1 - d/range)^power; com power = 3 isso é quase uma gaussiana de σ = range/3. Para imitar o
// desfoque do CSS, o miolo encolhe σ (fica escondido sob a janela, sem faixa sólida aparecendo) e o
// esmaecimento vai até 4σ.
inline constexpr int SHADOW_POWER = 3;
struct ShadowDraw {
    Rect   box;          // caixa inteira da sombra (miolo + esmaecimento)
    double range  = 0;   // largura do esmaecimento
    double inset  = 0;   // quanto o miolo fica para dentro da janela (+ spread): some do arredondamento
};
ShadowDraw shadowDraw(const Rect& win, const Shadow& s);

// quanto a sombra passa de cada lado da janela (para o Hyprland saber o que redesenhar), com folga.
// Cobre todas as camadas, ativas e inativas: trocar o foco não deixa resto de sombra na tela.
struct Extents {
    double top = 0, right = 0, bottom = 0, left = 0;
};
Extents shadowExtents(const Look& l);

} // namespace borders
