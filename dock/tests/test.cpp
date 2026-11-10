// Testes do modelo do hyprdock (sem dependência do Hyprland): `make test`.

#include "../src/model.hpp"

#include <cmath>
#include <cstdio>
#include <string>

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

using W = std::vector<std::pair<std::string, std::string>>;

// ---------- parseList ----------

static void testParseList() {
    CHECK(dock::parseList("").empty());
    CHECK(dock::parseList(" , ,").empty());
    const auto l = dock::parseList(" google-chrome,Alacritty ,, code ");
    CHECK(l.size() == 3);
    CHECK(l[0] == "google-chrome");
    CHECK(l[1] == "Alacritty");
    CHECK(l[2] == "code");
}

// ---------- buildItems ----------

static void testPinnedFirst() {
    std::vector<std::string> order;
    const auto items = dock::buildItems({"firefox", "Alacritty"}, W{{"0x2", "discord"}, {"0x1", "alacritty"}}, order);
    CHECK(items.size() == 3);
    CHECK(items[0].cls == "firefox" && items[0].pinned && items[0].windows.empty());
    CHECK(items[1].cls == "Alacritty" && items[1].pinned && items[1].windows.size() == 1); // casa sem diferenciar maiúsculas
    CHECK(items[2].cls == "discord" && !items[2].pinned);
}

static void testGroupsAndSorts() {
    std::vector<std::string> order;
    const auto items = dock::buildItems({}, W{{"0x30", "kitty"}, {"0x10", "kitty"}, {"0x20", "Kitty"}}, order);
    CHECK(items.size() == 1);
    CHECK(items[0].windows.size() == 3);
    CHECK(items[0].windows[0] == "0x10" && items[0].windows[2] == "0x30");
}

static void testStableOrder() {
    // a ordem dos não fixos não muda quando a lista do hyprctl vem embaralhada
    std::vector<std::string> order;
    dock::buildItems({}, W{{"0x1", "a"}, {"0x2", "b"}}, order);
    auto items = dock::buildItems({}, W{{"0x2", "b"}, {"0x1", "a"}, {"0x3", "c"}}, order);
    CHECK(items.size() == 3);
    CHECK(items[0].cls == "a" && items[1].cls == "b" && items[2].cls == "c");
    // b fechou: sai; quando volta, entra no fim
    items = dock::buildItems({}, W{{"0x1", "a"}, {"0x3", "c"}}, order);
    CHECK(items.size() == 2 && items[1].cls == "c");
    items = dock::buildItems({}, W{{"0x4", "b"}, {"0x1", "a"}, {"0x3", "c"}}, order);
    CHECK(items[2].cls == "b");
}

static void testHideRunning() {
    std::vector<std::string> order;
    const auto items = dock::buildItems({"a"}, W{{"0x1", "a"}, {"0x2", "b"}}, order, false);
    CHECK(items.size() == 1 && items[0].cls == "a" && items[0].windows.size() == 1);
}

static void testDuplicatePinnedAndEmptyClass() {
    std::vector<std::string> order;
    const auto items = dock::buildItems({"a", "A"}, W{{"0x1", ""}}, order);
    CHECK(items.size() == 1);
    CHECK(items[0].windows.empty());
}

// ---------- nextWindow ----------

static void testNextWindow() {
    const dock::Item it{"a", false, {"0x1", "0x2", "0x3"}};
    CHECK(dock::nextWindow(it, "") == "0x1");
    CHECK(dock::nextWindow(it, "0x9") == "0x1"); // foco em outro app
    CHECK(dock::nextWindow(it, "0x1") == "0x2");
    CHECK(dock::nextWindow(it, "0x3") == "0x1"); // dá a volta
    CHECK(dock::nextWindow(dock::Item{"b", true, {}}, "0x1").empty());
}

static void testTargetWindow() {
    const dock::Item it{"a", false, {"0x1", "0x2"}};
    CHECK(dock::targetWindow(it, "0x2") == "0x2"); // a focada, se for do app
    CHECK(dock::targetWindow(it, "0x9") == "0x1");
    CHECK(dock::targetWindow(dock::Item{"b", true, {}}, "0x1").empty());
}

// ---------- fixos ----------

static void testPins() {
    const std::vector<std::string> config = {"firefox", "Alacritty"};
    dock::PinOverrides             ov;
    CHECK(dock::effectivePins(config, ov) == config);

    dock::setPinned(ov, config, "Discord", true); // fixa algo fora da config
    dock::setPinned(ov, config, "ALACRITTY", false); // desafixa algo da config
    auto pins = dock::effectivePins(config, ov);
    CHECK(pins.size() == 2 && pins[0] == "firefox" && pins[1] == "discord");

    // desfazer volta exatamente ao estado da config, sem sobras no arquivo
    dock::setPinned(ov, config, "discord", false);
    dock::setPinned(ov, config, "alacritty", true);
    CHECK(ov.added.empty() && ov.removed.empty());
    CHECK(dock::effectivePins(config, ov) == config);

    // fixar duas vezes não duplica
    dock::setPinned(ov, config, "x", true);
    dock::setPinned(ov, config, "X", true);
    CHECK(ov.added.size() == 1);
}

static void testPinsFile() {
    dock::PinOverrides ov;
    ov.added   = {"discord", "spotify"};
    ov.removed = {"firefox"};
    const auto back = dock::parsePins(dock::serializePins(ov));
    CHECK(back.added == ov.added && back.removed == ov.removed);

    // linhas estranhas são ignoradas; espaços e maiúsculas normalizados
    const auto p = dock::parsePins("\n+  Kitty \n?lixo\n-\n+\n-Firefox\n+kitty\r\n");
    CHECK(p.added.size() == 1 && p.added[0] == "kitty");
    CHECK(p.removed.size() == 1 && p.removed[0] == "firefox");
    CHECK(dock::parsePins("").added.empty());
}

// ---------- stripFieldCodes ----------

static void testStripFieldCodes() {
    CHECK(dock::stripFieldCodes("/usr/bin/google-chrome-stable %U") == "/usr/bin/google-chrome-stable");
    CHECK(dock::stripFieldCodes("code --new-window %F") == "code --new-window");
    CHECK(dock::stripFieldCodes("echo 100%% %f done") == "echo 100%  done");
    CHECK(dock::stripFieldCodes("alacritty") == "alacritty");
    CHECK(dock::stripFieldCodes("x %") == "x"); // % solto no fim não lê fora da string
}

// ---------- geometria ----------

static void testGeometry() {
    const auto g = dock::geometry(4, 1920, 1080, 48, 8);
    const double cell = 48 + 2 * dock::PAD;
    CHECK(near(g.k, 1));
    CHECK(near(g.w, 4 * cell + 3 * dock::GAP + 2 * dock::BARPAD));
    CHECK(near(g.x, (1920 - g.w) / 2));
    CHECK(near(g.y + g.h, 1080 - 8)); // encostada embaixo, a `margin` da borda
}

static void testGeometryShrinks() {
    const auto g = dock::geometry(60, 1280, 720, 64, 8);
    CHECK(g.k < 1);
    CHECK(near(g.w, 1280 - 80));
    const auto last = dock::cell(g, 59);
    CHECK(near(last.x + last.size + dock::BARPAD * g.k, g.x + g.w));
    CHECK(near(g.y + g.h, 720 - 8));
}

static void testHit() {
    const size_t n = 5;
    const auto   g = dock::geometry(n, 1920, 1080, 48, 8);
    for (size_t i = 0; i < n; i++) {
        const auto c = dock::cell(g, i);
        CHECK(dock::hit(g, n, c.x + c.size / 2, c.y + c.size / 2) == (int)i);
    }
    const auto c0 = dock::cell(g, 0), c1 = dock::cell(g, 1);
    CHECK(dock::hit(g, n, (c0.x + c0.size + c1.x) / 2, c0.y + 5) == -1);
    CHECK(dock::hit(g, n, g.x + g.w / 2, g.y - 1) == -2);
    CHECK(dock::hit(g, n, g.x + g.w / 2, 1080 - 2) == -2); // abaixo da dock, na margem
}

// ---------- menu ----------

static void testMenu() {
    const dock::Frame f{dock::Edge::Bottom, 1920, 1080};
    const auto        g = dock::geometry(3, 1920, 1080, 48, 8);
    const auto        c = dock::cell(g, 1);
    const dock::Box   cb{c.x, c.y, c.size, c.size}, pb{g.x, g.y, g.w, g.h};
    const auto        m = dock::menuGeometry(f, cb, pb, 4);
    CHECK(near(m.x + m.w / 2, c.x + c.size / 2)); // centralizado no item
    CHECK(near(m.y + m.h, g.y - dock::MENU_GAP));  // logo acima da dock
    CHECK(near(m.h, 4 * dock::MENU_ROW + 2 * dock::MENU_PAD));

    for (int r = 0; r < 4; r++)
        CHECK(dock::menuHit(m, 4, m.x + 10, m.y + dock::MENU_PAD + (r + 0.5) * dock::MENU_ROW) == r);
    CHECK(dock::menuHit(m, 4, m.x + 10, m.y + 2) == -1);  // no padding de cima
    CHECK(dock::menuHit(m, 4, m.x - 1, m.y + 20) == -2);
    CHECK(dock::menuHit(m, 4, m.x + 10, m.y + m.h + 1) == -2);

    // item na beirada: o menu não sai do monitor
    const auto edge = dock::menuGeometry(f, dock::Box{0, 1000, 60, 60}, dock::Box{0, 990, 300, 80}, 2);
    CHECK(edge.x >= 4);
    const auto right = dock::menuGeometry(f, dock::Box{1900, 1000, 60, 60}, dock::Box{1600, 990, 320, 80}, 2);
    CHECK(right.x + right.w <= 1920 - 4 + 1e-9);
}

// ---------- bordas ----------

static void testParseEdge() {
    CHECK(dock::parseEdge("left") == dock::Edge::Left);
    CHECK(dock::parseEdge(" Right ") == dock::Edge::Right);
    CHECK(dock::parseEdge("bottom") == dock::Edge::Bottom);
    CHECK(dock::parseEdge("") == dock::Edge::Bottom);
    CHECK(dock::parseEdge("top") == dock::Edge::Bottom); // não suportado: cai no padrão
}

static void testEdgeTransform() {
    for (auto e : {dock::Edge::Bottom, dock::Edge::Left, dock::Edge::Right}) {
        const dock::Frame f{e, 1920, 1080};
        const auto        g = dock::geometry(5, f.w(), f.h(), 48, 8);
        const auto        p = dock::toScreen(f, {g.x, g.y, g.w, g.h});
        // o painel fica dentro do monitor, a `margin` da sua borda, centrado ao longo dela
        CHECK(p.x >= 0 && p.y >= 0 && p.x + p.w <= 1920 + 1e-9 && p.y + p.h <= 1080 + 1e-9);
        if (e == dock::Edge::Bottom)
            CHECK(near(p.y + p.h, 1080 - 8) && near(p.x + p.w / 2, 960));
        if (e == dock::Edge::Left)
            CHECK(near(p.x, 8) && near(p.y + p.h / 2, 540) && p.h > p.w);
        if (e == dock::Edge::Right)
            CHECK(near(p.x + p.w, 1920 - 8) && near(p.y + p.h / 2, 540) && p.h > p.w);

        // ida e volta: o centro de cada item, convertido para o monitor e de volta, acerta o item
        for (size_t i = 0; i < 5; i++) {
            const auto c  = dock::cell(g, i);
            const auto sc = dock::toScreen(f, {c.x, c.y, c.size, c.size});
            double     dx = 0, dy = 0;
            dock::toDock(f, sc.x + sc.w / 2, sc.y + sc.h / 2, dx, dy);
            CHECK(dock::hit(g, 5, dx, dy) == (int)i);
        }
        // "para dentro" no espaço da dock (y menor) aponta para o centro da tela
        const auto a = dock::toScreen(f, {g.x, g.y, 1, 1}), b = dock::toScreen(f, {g.x, g.y - 50, 1, 1});
        CHECK(std::abs(b.x + 0.5 - 960) + std::abs(b.y + 0.5 - 540) < std::abs(a.x + 0.5 - 960) + std::abs(a.y + 0.5 - 540));
    }
}

static void testBesideItem() {
    const dock::Box cell{20, 500, 60, 60}, panel{8, 300, 76, 400};
    const auto      l = dock::besideItem({dock::Edge::Left, 1920, 1080}, cell, panel, 100, 30, 8);
    CHECK(near(l.x, panel.x + panel.w + 8) && near(l.y + l.h / 2, cell.y + cell.h / 2)); // à direita, centrado
    const dock::Box rcell{1840, 500, 60, 60}, rpanel{1836, 300, 76, 400};
    const auto      r = dock::besideItem({dock::Edge::Right, 1920, 1080}, rcell, rpanel, 100, 30, 8);
    CHECK(near(r.x + r.w, rpanel.x - 8) && near(r.y + r.h / 2, rcell.y + rcell.h / 2)); // à esquerda
    // item no topo da tela: o tooltip não sai pelo alto
    const auto top = dock::besideItem({dock::Edge::Left, 1920, 1080}, dock::Box{20, 0, 60, 60}, panel, 100, 200, 8);
    CHECK(top.y >= 4);
}

// ---------- ampliação ----------

static void testMagnifyOff() {
    // sem ampliação (mag 1 ou level 0) o layout é o de sempre
    const size_t n = 5;
    const auto   g = dock::geometry(n, 1920, 1080, 48, 8);
    for (auto [mag, level] : {std::pair{1.0, 1.0}, std::pair{1.8, 0.0}}) {
        const auto l = dock::magnify(g, n, g.x + g.w / 2, mag, level);
        CHECK(near(l.panel.x, g.x) && near(l.panel.w, g.w));
        for (size_t i = 0; i < n; i++) {
            const auto c = dock::cell(g, i);
            CHECK(near(l.cells[i].x, c.x) && near(l.cells[i].y, c.y) && near(l.cells[i].size, c.size));
        }
    }
}

static void testMagnifyPeak() {
    const size_t n  = 7;
    const auto   g  = dock::geometry(n, 1920, 1080, 48, 8);
    const auto   c3 = dock::cell(g, 3);
    const auto   l  = dock::magnify(g, n, c3.x + c3.size / 2, 2.0, 1);
    CHECK(near(l.cells[3].size, c3.size * 2)); // o do cursor dobra
    CHECK(l.cells[2].size < l.cells[3].size && l.cells[2].size > c3.size);
    CHECK(near(l.cells[2].size, l.cells[4].size)); // simétrico
    CHECK(near(l.cells[0].size, c3.size));         // longe: tamanho normal
    // todos crescem para cima: a base continua alinhada com a da dock
    for (auto& c : l.cells)
        CHECK(near(c.y + c.size, c3.y + c3.size));
    // o painel alarga o mesmo que a fileira
    double sum = 0;
    for (auto& c : l.cells)
        sum += c.size;
    CHECK(near(l.panel.w, sum + (n - 1) * dock::GAP + 2 * dock::BARPAD));
}

static void testMagnifyAnchored() {
    // em qualquer ponto da fileira, o item sob o cursor continua sob o cursor depois de ampliar
    const size_t n = 6;
    const auto   g = dock::geometry(n, 1920, 1080, 48, 8);
    for (double mx = g.x + 1; mx < g.x + g.w; mx += 3) {
        const int before = dock::hit(g, n, mx, g.y + g.h / 2);
        if (before < 0)
            continue;
        const auto l = dock::magnify(g, n, mx, 1.8, 1);
        CHECK(dock::hit(l, mx, l.cells[before].y + l.cells[before].size / 2) == before);
    }
    // cursor antes do primeiro item: borda esquerda parada; depois do último: direita parada
    const auto left = dock::magnify(g, n, g.x + 2, 1.8, 1);
    CHECK(near(left.panel.x, g.x));
    const auto right = dock::magnify(g, n, g.x + g.w - 2, 1.8, 1);
    CHECK(near(right.panel.x + right.panel.w, g.x + g.w));
}

static void testMagnifyHit() {
    const size_t n = 4;
    const auto   g = dock::geometry(n, 1920, 1080, 48, 8);
    const auto   c = dock::cell(g, 1);
    const auto   l = dock::magnify(g, n, c.x + c.size / 2, 2.0, 1);
    // o topo do ícone ampliado fica acima do painel e ainda conta como o item
    CHECK(l.cells[1].y < g.y);
    CHECK(dock::hit(l, c.x + c.size / 2, l.cells[1].y + 2) == 1);
    CHECK(dock::hit(l, l.cells[0].x + 1, l.cells[1].y + 2) == -2); // acima do vizinho menor
    CHECK(dock::hit(dock::Layout{g, {}}, g.x + 1, g.y + 1) == -1);
}

// ---------- pulo ----------

static void testBounce() {
    const double cell = 60, top = cell * dock::BOUNCE_HEIGHT;
    CHECK(near(dock::bounceOffset(0, cell), 0));
    CHECK(near(dock::bounceOffset(-5, cell), 0));
    CHECK(near(dock::bounceOffset(dock::BOUNCE_PERIOD / 2, cell), top));      // topo no meio do pulo
    CHECK(near(dock::bounceOffset(dock::BOUNCE_PERIOD, cell), 0));            // pousa no fim
    CHECK(near(dock::bounceOffset(dock::BOUNCE_PERIOD * 2.5, cell), top));    // repete igual
    for (double t = 0; t < 3 * dock::BOUNCE_PERIOD; t += 7)
        CHECK(dock::bounceOffset(t, cell) >= 0 && dock::bounceOffset(t, cell) <= top + 1e-9);
    // gravidade: sobe mais rápido no começo do que perto do topo
    CHECK(dock::bounceOffset(60, cell) - dock::bounceOffset(0, cell) > dock::bounceOffset(300, cell) - dock::bounceOffset(240, cell));

    // parar no meio do pulo espera ele pousar; parar no chão para na hora
    CHECK(near(dock::bounceLanding(dock::BOUNCE_PERIOD * 1.3), dock::BOUNCE_PERIOD * 2));
    CHECK(near(dock::bounceLanding(dock::BOUNCE_PERIOD), dock::BOUNCE_PERIOD));
    CHECK(near(dock::bounceOffset(dock::bounceLanding(1234), cell), 0));
}

int main() {
    testParseList();
    testPinnedFirst();
    testGroupsAndSorts();
    testStableOrder();
    testHideRunning();
    testDuplicatePinnedAndEmptyClass();
    testNextWindow();
    testTargetWindow();
    testPins();
    testPinsFile();
    testStripFieldCodes();
    testGeometry();
    testGeometryShrinks();
    testHit();
    testMenu();
    testParseEdge();
    testEdgeTransform();
    testBesideItem();
    testMagnifyOff();
    testMagnifyPeak();
    testMagnifyAnchored();
    testMagnifyHit();
    testBounce();

    std::printf("%d/%d checagens ok\n", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
