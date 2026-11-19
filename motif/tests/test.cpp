// Testes da geometria da moldura (sem Hyprland): make -C motif test
#include "../src/geometry.hpp"

#include <cstdio>
#include <cstdlib>

using namespace motif;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::printf("FALHOU %s:%d: %s\n", __FILE__, __LINE__, #c);                                                 \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

int main() {
    const double W = 400, H = 300;
    const int    B = 4, T = 18;
    const auto   l = layout(W, H, B, T);

    // cliente: dentro da borda e abaixo da barra
    CHECK(l.client.x == B && l.client.y == B + T);
    CHECK(l.client.w == W - 2 * B && l.client.h == H - 2 * B - T);
    CHECK(l.bar.y == B && l.bar.h == T);

    // botões: menu à esquerda, min/max à direita, texto entre eles
    CHECK(l.menu.x == B && l.menu.w == T);
    CHECK(l.maximize.x + l.maximize.w == W - B);
    CHECK(l.minimize.x + l.minimize.w == l.maximize.x);
    CHECK(l.text.x == l.menu.x + T);
    CHECK(l.text.x + l.text.w == l.minimize.x);

    // hit-test
    CHECK(hit(l, W, H, -1, 10) == Part::None);
    CHECK(hit(l, W, H, W, 10) == Part::None);
    CHECK(hit(l, W, H, 1, 1) == Part::Frame);
    CHECK(hit(l, W, H, W / 2, 1) == Part::Frame);
    CHECK(hit(l, W, H, W / 2, H - 1) == Part::Frame);
    CHECK(hit(l, W, H, 1, H / 2) == Part::Frame);
    CHECK(hit(l, W, H, W / 2, B + T / 2) == Part::Title);
    CHECK(hit(l, W, H, B + T / 2, B + T / 2) == Part::Menu);
    CHECK(hit(l, W, H, W - B - T / 2, B + T / 2) == Part::Maximize);
    CHECK(hit(l, W, H, W - B - T - T / 2, B + T / 2) == Part::Minimize);
    CHECK(hit(l, W, H, W / 2, H / 2) == Part::Client);
    CHECK(hit(l, W, H, B, B + T) == Part::Client);     // primeiro pixel do cliente
    CHECK(hit(l, W, H, B - 1, B + T) == Part::Frame);  // um px antes: ainda moldura
    CHECK(hit(l, W, H, W - B, H / 2) == Part::Frame);  // primeiro px da borda direita

    // estreita: só o menu cabe; mais estreita ainda: nenhum botão
    const auto n = layout(60, 100, B, T);
    CHECK(n.menu.w == T && n.minimize.w == 0 && n.maximize.w == 0);
    CHECK(n.text.w == n.bar.w - T);
    const auto z = layout(20, 100, B, T);
    CHECK(z.menu.w == 0 && z.text.w == z.bar.w);

    // degenerada: sem cliente nem crash
    const auto d = layout(5, 5, B, T);
    CHECK(d.client.w == 0 && d.client.h == 0);
    CHECK(hit(d, 5, 5, 2, 2) != Part::Client);

    std::puts("ok");
    return 0;
}
