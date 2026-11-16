// Testes do modelo do hyprpager (sem dependência do Hyprland): `make test`.

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

static void testLayout() {
    const auto l = pager::layout(4, 60);
    CHECK(l.screens.size() == 4 && l.brackets.size() == 4);
    CHECK(near(l.screenH, 60 / pager::SCREEN_ASPECT));
    for (size_t i = 0; i < 4; i++) {
        const auto& s = l.screens[i];
        CHECK(s.x > 0 && s.y > 0);                         // espaço para o engate e o cano de cima
        CHECK(s.x + s.w <= l.w && s.y + s.h <= l.h);        // tudo dentro do pager
        CHECK(l.brackets[i].x > s.x + s.w);                // placa à direita da tela
        CHECK(l.brackets[i].x + l.brackets[i].w <= l.w + 1e-9);
        if (i)
            CHECK(near(s.y - l.screens[i - 1].y, l.screenH + 60 * pager::GAP)); // espaçamento constante
    }
    CHECK(pager::layout(0, 60).screens.empty());
    // escala: o dobro da tela dobra tudo
    const auto l2 = pager::layout(4, 120);
    CHECK(near(l2.w, l.w * 2) && near(l2.h, l.h * 2));
}

static pager::Placement at(pager::Edge e, double margin, pager::Align a = pager::Align::Center, double offset = 0) {
    return {e, a, margin, offset};
}

static void testPlace() {
    using pager::Align;
    using pager::Edge;
    const auto l = pager::layout(4, 60);
    const auto a = pager::place(l, at(Edge::Left, 10), 1920, 1080);
    CHECK(near(a.x, 10) && near(a.y, (1080 - l.h) / 2));
    const auto b = pager::place(l, at(Edge::Right, 10), 1920, 1080);
    CHECK(near(b.x + b.w, 1910));
    // pager mais alto que a tela: começa no topo
    const auto big = pager::layout(40, 60);
    CHECK(near(pager::place(big, at(Edge::Left, 10), 1920, 1080).y, 0));
    CHECK(near(pager::place(big, at(Edge::Left, 10, Align::Bottom), 1920, 1080).y, 0));
    // área reservada (dock na esquerda, barra em cima): fica do lado de dentro, centrado no espaço livre
    const auto c = pager::place(l, at(Edge::Left, 10), 1920, 1080, {28, 0, 0, 80});
    CHECK(near(c.x, 90) && near(c.y, 28 + (1080 - 28 - l.h) / 2));
    const auto d = pager::place(l, at(Edge::Right, 10), 1920, 1080, {0, 70, 0, 0});
    CHECK(near(d.x + d.w, 1920 - 70 - 10));
    // em cima / embaixo: a `margin` px da área reservada
    CHECK(near(pager::place(l, at(Edge::Left, 10, Align::Top), 1920, 1080, {28, 0, 0, 0}).y, 38));
    CHECK(near(pager::place(l, at(Edge::Left, 10, Align::Bottom), 1920, 1080, {0, 0, 50, 0}).y, 1080 - 50 - 10 - l.h));
    // deslocamento: soma depois de alinhar, preso na tela
    CHECK(near(pager::place(l, at(Edge::Left, 10, Align::Top, 100), 1920, 1080).y, 110));
    CHECK(near(pager::place(l, at(Edge::Left, 10, Align::Center, -40), 1920, 1080).y, (1080 - l.h) / 2 - 40));
    CHECK(near(pager::place(l, at(Edge::Left, 10, Align::Top, -500), 1920, 1080).y, 0));
    CHECK(near(pager::place(l, at(Edge::Left, 10, Align::Bottom, 500), 1920, 1080).y, 1080 - l.h));
    CHECK(pager::parseEdge("RIGHT") == pager::Edge::Right);
    CHECK(pager::parseEdge("x") == pager::Edge::Left);
    CHECK(pager::parseAlign("Top") == Align::Top);
    CHECK(pager::parseAlign("bottom") == Align::Bottom);
    CHECK(pager::parseAlign("") == Align::Center);
}

static void testMonitor() {
    CHECK(pager::monitorMatches("", "DP-1"));
    CHECK(pager::monitorMatches(" , ", "DP-1"));
    CHECK(pager::monitorMatches("DP-1", "DP-1"));
    CHECK(!pager::monitorMatches("DP-1", "HDMI-A-1"));
    CHECK(pager::monitorMatches("DP-1, HDMI-A-1", "HDMI-A-1"));
    CHECK(!pager::monitorMatches("DP-1,HDMI-A-1", "eDP-1"));
}

static void testHit() {
    const auto l = pager::layout(3, 60);
    const auto s = l.screens[1];
    CHECK(pager::hitScreen(l, s.x + 5, s.y + 5) == 1);
    CHECK(pager::hitScreen(l, s.x - l.tube / 2, s.y + 5) == 1); // na moldura
    const auto b = l.brackets[2];
    CHECK(pager::hitScreen(l, b.x + b.w / 2, b.y + b.h / 2) == 2); // na placa
    CHECK(pager::hitScreen(l, -5, -5) == -1);
    // no vão entre duas telas (fora das placas), nada
    const double mid = (l.screens[0].y + l.screenH + l.screens[1].y) / 2 + l.tube; // abaixo do meio do vão
    CHECK(pager::hitScreen(l, l.screens[0].x + 2, mid) == -1);
}

static void testFit() {
    const pager::Box glass{0, 0, 100, 80};
    const auto       a = pager::fitMonitor(glass, 1920, 1080); // 16:9 num vidro mais quadrado: faixas em cima e embaixo
    CHECK(near(a.w / a.h, 1920.0 / 1080.0));
    CHECK(a.x >= 0 && a.x + a.w <= 100 && a.y > 0 && a.y + a.h < 80);
    CHECK(near(a.x + a.w / 2, 50) && near(a.y + a.h / 2, 40)); // centrado
    const auto b = pager::fitMonitor(glass, 1080, 1920);       // monitor em pé: faixas dos lados
    CHECK(near(b.w / b.h, 1080.0 / 1920.0) && b.x > 0);
}

static void testThumb() {
    const pager::Box area{10, 20, 192, 108}; // 1920x1080 / 10
    auto             t = pager::thumb(area, 1920, 1080, {960, 540, 480, 270});
    CHECK(near(t.x, 106) && near(t.y, 74) && near(t.w, 48) && near(t.h, 27));
    // passando da borda: cortada
    t = pager::thumb(area, 1920, 1080, {1800, 1000, 400, 400});
    CHECK(near(t.x + t.w, area.x + area.w) && near(t.y + t.h, area.y + area.h));
    // toda fora: tamanho zero
    t = pager::thumb(area, 1920, 1080, {-500, 0, 100, 100});
    CHECK(near(t.w, 0));
}

static void testLabel() {
    CHECK(pager::workspaceLabel(3, "3") == "");
    CHECK(pager::workspaceLabel(3, "") == "");
    CHECK(pager::workspaceLabel(3, "web") == "web");
}

int main() {
    testLayout();
    testPlace();
    testMonitor();
    testHit();
    testFit();
    testThumb();
    testLabel();

    std::printf("%d/%d checagens ok\n", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
