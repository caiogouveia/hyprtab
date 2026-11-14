// Testes do modelo do hyprborders (sem dependência do Hyprland): `make test`.

#include "../src/model.hpp"

#include <cmath>
#include <cstdio>

static int g_fails = 0, g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fails++;                                                    \
            std::printf("FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static bool near(double a, double b) {
    return std::abs(a - b) < 1e-9;
}

static void testShadowDraw() {
    // blur 40 -> σ 20: miolo 20 px para dentro da janela, esmaecimento de 80 px
    const auto d = borders::shadowDraw({100, 200, 800, 600}, {40, 15, 0, 0.5});
    CHECK(near(d.range, 80) && near(d.inset, 20));
    CHECK(near(d.box.x, 100 - 60) && near(d.box.w, 800 + 120));
    CHECK(near(d.box.y, 200 - 60 + 15)); // descida
    // o miolo (caixa menos o esmaecimento) fica dentro da janela deslocada: nada sólido aparece em volta
    CHECK(d.box.x + d.range > 100 && d.box.x + d.box.w - d.range < 900);

    // spread empurra o miolo para fora
    const auto sp = borders::shadowDraw({0, 0, 100, 100}, {8, 0, 10, 1});
    CHECK(near(sp.inset, -6));

    // janela minúscula: o miolo não vira caixa negativa
    const auto t = borders::shadowDraw({0, 0, 10, 10}, {70, 0, 0, 1});
    CHECK(t.box.w - 2 * t.range > 0);
}

static void testLayers() {
    const borders::Look l;
    const auto          on  = borders::shadowLayers(l, true);
    const auto          off = borders::shadowLayers(l, false);
    // ambiente primeiro (embaixo), contato depois: o contato é bem mais curto
    CHECK(on[0].blur > on[1].blur && off[0].blur > off[1].blur);
    // como no macOS: a janela em foco tem a sombra maior, mais deslocada e mais escura
    CHECK(on[0].blur > off[0].blur);
    CHECK(on[0].offsetY > off[0].offsetY);
    CHECK(on[0].alpha > off[0].alpha);
    CHECK(on[1].alpha >= off[1].alpha);

    borders::Look x;
    x.ambientActive = {-5, 0, 0, 3}; // valores absurdos da config são contidos
    const auto c    = borders::shadowLayers(x, true);
    CHECK(near(c[0].blur, 0) && near(c[0].alpha, 1));
}

static void testExtents() {
    const borders::Look l;
    const auto          e = borders::shadowExtents(l);
    // a área de redesenho cobre todas as camadas, ativas e inativas
    for (bool active : {true, false})
        for (const auto& s : borders::shadowLayers(l, active)) {
            const auto b = borders::shadowDraw({0, 0, 300, 200}, s).box;
            CHECK(e.left >= -b.x && e.top >= -b.y);
            CHECK(e.right >= b.x + b.w - 300 && e.bottom >= b.y + b.h - 200);
        }
    CHECK(e.bottom > e.top); // deslocada para baixo

    borders::Look big;
    big.ambientActive.offsetY = 400; // deslocamento maior que o alcance: nada passa em cima
    CHECK(borders::shadowExtents(big).top >= 0);
}

int main() {
    testShadowDraw();
    testLayers();
    testExtents();
    std::printf("%d/%d checagens ok\n", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
