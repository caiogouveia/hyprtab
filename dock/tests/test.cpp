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

// ---------- reordenar ----------

static void testMoveTo() {
    const std::vector<std::string> l = {"a", "b", "c", "d"};
    CHECK((dock::moveTo(l, "a", 2) == std::vector<std::string>{"b", "c", "a", "d"}));
    CHECK((dock::moveTo(l, "D", 0) == std::vector<std::string>{"d", "a", "b", "c"}));
    CHECK((dock::moveTo(l, "b", 99) == std::vector<std::string>{"a", "c", "d", "b"})); // além do fim: vai pro fim
    CHECK(dock::moveTo(l, "zzz", 1) == l);                                              // não está na lista
}

static void testReorderPins() {
    const std::vector<std::string> config = {"firefox", "Alacritty", "code"};
    dock::PinOverrides             ov;
    dock::reorderPin(ov, config, "code", 0);
    auto pins = dock::effectivePins(config, ov);
    CHECK((pins == std::vector<std::string>{"code", "firefox", "Alacritty"}));

    // fixar um novo depois de reordenar: entra no fim, sem bagunçar a ordem salva
    dock::setPinned(ov, config, "discord", true);
    pins = dock::effectivePins(config, ov);
    CHECK(pins.size() == 4 && pins[0] == "code" && pins[3] == "discord");

    // remover um da ordem: some, o resto mantém a ordem
    dock::setPinned(ov, config, "firefox", false);
    pins = dock::effectivePins(config, ov);
    CHECK((pins == std::vector<std::string>{"code", "Alacritty", "discord"}));

    // a ordem sobrevive ao arquivo
    const auto back = dock::parsePins(dock::serializePins(ov));
    CHECK(dock::effectivePins(config, back) == pins);
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
    CHECK(dock::hit(dock::Layout{g, {}, {}}, g.x + 1, g.y + 1) == -1);
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

static void testNewlyOpened() {
    using I = dock::Item;
    const std::vector<I> before = {I{"firefox", true, {}}, I{"Alacritty", true, {"0x1"}}, I{"discord", false, {"0x2"}}};
    const std::vector<I> after  = {I{"firefox", true, {"0x3"}},          // fixo fechado que abriu
                                   I{"Alacritty", true, {"0x1", "0x4"}}, // já aberto, ganhou outra janela
                                   I{"discord", false, {"0x2"}},
                                   I{"spotify", false, {"0x5"}}};        // novo, não estava na dock
    const auto n = dock::newlyOpened(before, after);
    CHECK((n == std::vector<std::string>{"firefox", "spotify"}));
    CHECK(dock::newlyOpened(after, after).empty());
    CHECK(dock::newlyOpened(after, before).empty()); // fechar não é abrir
}

// ---------- arrastar ----------

static void testDropIndex() {
    const size_t n = 6;
    const auto   g = dock::geometry(n, 1920, 1080, 48, 8);
    for (size_t i = 0; i < n; i++) {
        const auto c = dock::cell(g, i);
        CHECK(dock::dropIndex(g, n, c.x + c.size / 2, 0, n - 1) == i); // sobre o item i
    }
    CHECK(dock::dropIndex(g, n, g.x - 500, 0, n - 1) == 0);      // fora pela esquerda
    CHECK(dock::dropIndex(g, n, g.x + 5000, 0, n - 1) == n - 1); // fora pela direita
    const auto c4 = dock::cell(g, 4);
    CHECK(dock::dropIndex(g, n, c4.x + 1, 0, 2) == 2);           // preso aos fixos [0, 2]
}

static void testDragSlot() {
    // arrastando o 1 para a posição 3 numa fileira de 5: 0 fica, 2->1, 3->2, (3 vazio), 4->4
    CHECK(dock::dragSlot(0, 1, 3) == 0);
    CHECK(dock::dragSlot(2, 1, 3) == 1);
    CHECK(dock::dragSlot(3, 1, 3) == 2);
    CHECK(dock::dragSlot(4, 1, 3) == 4);
    // arrastando o 3 para a posição 0: todos antes dele andam uma casa
    CHECK(dock::dragSlot(0, 3, 0) == 1);
    CHECK(dock::dragSlot(2, 3, 0) == 3);
    CHECK(dock::dragSlot(4, 3, 0) == 4);
    // posição de origem: ninguém se mexe
    for (size_t i : {0, 1, 3, 4})
        CHECK(dock::dragSlot(i, 2, 2) == i);
}

static void testDragOutside() {
    const auto   g  = dock::geometry(5, 1920, 1080, 48, 8);
    const double cs = 48 + 2 * dock::PAD;
    CHECK(!dock::dragOutside(g, g.x + g.w / 2, g.y + g.h / 2)); // em cima da dock
    CHECK(!dock::dragOutside(g, g.x + g.w / 2, g.y - cs + 1));  // pouco acima: ainda reordena
    CHECK(dock::dragOutside(g, g.x + g.w / 2, g.y - cs - 1));   // acima de uma célula: remove
    CHECK(dock::dragOutside(g, g.x - cs - 1, g.y + 5));         // além da ponta esquerda
    CHECK(dock::dragOutside(g, g.x + g.w + cs + 1, g.y + 5));   // além da ponta direita
    CHECK(!dock::dragOutside(g, g.x - 5, g.y + 5));             // logo ao lado: ainda reordena
}

// ---------- entrada ----------

static void testEnter() {
    const double d = 100;
    CHECK(near(dock::enterOffset(0, d), d));             // começa escondida além da borda
    CHECK(near(dock::enterOffset(-5, d), d));
    CHECK(near(dock::enterOffset(dock::ENTER_MS, d), 0)); // termina no lugar
    CHECK(near(dock::enterOffset(dock::ENTER_MS * 3, d), 0));
    // anda rápido no começo, devagar no fim
    CHECK(d - dock::enterOffset(dock::ENTER_MS * 0.2, d) > 40);
    // passa um pouco do ponto (deslocamento negativo) e volta, sem exagero
    double minOff = 0;
    for (double ms = 0; ms <= dock::ENTER_MS; ms += 2) {
        const double o = dock::enterOffset(ms, d);
        minOff         = std::min(minOff, o);
        CHECK(o <= d + 1e-9);
    }
    CHECK(minOff < 0 && minOff > -0.15 * d);
}

// ---------- puff ----------

static void testPoof() {
    const double icon = 48;
    CHECK(dock::poofPuffs(1, icon).empty());
    CHECK(dock::poofPuffs(-0.1, icon).empty());
    CHECK(dock::poofPuffs(0.3, icon).size() == dock::poofPuffs(0.3, icon).size()); // determinístico

    // começo: clarão + todas as partículas; fim: só sobram as de vida longa
    const auto a = dock::poofPuffs(0.02, icon);
    CHECK(a.size() == (size_t)dock::POOF_PARTICLES + 1 && a.front().variant == -1);
    CHECK(dock::poofPuffs(0.95, icon).size() < (size_t)dock::POOF_PARTICLES);

    double spreadA = 0, spreadB = 0, yA = 0, yB = 0;
    size_t nA = 0, nB = 0;
    for (double t = 0; t < 1; t += 0.005)
        for (auto& p : dock::poofPuffs(t, icon)) {
            CHECK(p.alpha >= 0 && p.alpha <= 1 && p.size > 0);
            CHECK(std::hypot(p.dx, p.dy) <= 2 * icon); // a nuvem fica perto do ponto
            CHECK(p.variant >= -1 && p.variant < dock::POOF_VARIANTS);
            if (p.variant < 0)
                continue;
            if (t < 0.15)
                spreadA += std::hypot(p.dx, p.dy), yA += p.dy, nA++;
            else if (t > 0.5 && t < 0.6)
                spreadB += std::hypot(p.dx, p.dy), yB += p.dy, nB++;
        }
    CHECK(spreadB / nB > spreadA / nA); // se espalha
    CHECK(yB / nB < yA / nA);           // e sobe (y para cima é menor)

    // o ícone encolhe e some antes da fumaça
    CHECK(near(dock::poofIconScale(0), 1) && near(dock::poofIconAlpha(0), 1));
    CHECK(dock::poofIconScale(0.2) < 1 && dock::poofIconAlpha(0.2) < 1);
    CHECK(near(dock::poofIconAlpha(dock::POOF_ICON_END), 0) && near(dock::poofIconScale(0.5), 0));
}

static void testSeparator() {
    // inválidos (0, >= n) e repetidos são ignorados
    CHECK(dock::geometry(4, 1920, 1080, 48, 8, {0}).seps.empty());
    CHECK(dock::geometry(4, 1920, 1080, 48, 8, {4}).seps.empty());
    CHECK(dock::geometry(4, 1920, 1080, 48, 8, {2, 2}).seps.size() == 1);
    CHECK(near(dock::geometry(4, 1920, 1080, 48, 8, {4}).w, dock::geometry(4, 1920, 1080, 48, 8).w));

    const size_t n = 5;
    const auto   g = dock::geometry(n, 1920, 1080, 48, 8, {2}); // 2 fixos | 3 abertos
    const auto   p = dock::geometry(n, 1920, 1080, 48, 8);
    CHECK(g.seps.size() == 1 && g.seps[0] == 2);
    CHECK(near(g.w, p.w + dock::SEP_W));                                    // o vão alarga o painel
    CHECK(near(dock::cell(g, 1).x, dock::cell(p, 1).x - dock::SEP_W / 2)); // centralizado: tudo recua meio vão
    CHECK(near(dock::cell(g, 2).x - (dock::cell(g, 1).x + dock::cell(g, 1).size), dock::GAP + dock::SEP_W));
    // no vão do separador não há item
    const double gapMid = dock::cell(g, 2).x - dock::SEP_W / 2;
    CHECK(dock::hit(g, n, gapMid, g.y + g.h / 2) == -1);

    // sem zoom, o layout é o da geometria e a linha fica no meio do vão
    const auto l0 = dock::magnify(g, n, 0, 1, 0);
    for (size_t i = 0; i < n; i++)
        CHECK(near(l0.cells[i].x, dock::cell(g, i).x));
    const double a = l0.cells[1].x + l0.cells[1].size, b = l0.cells[2].x;
    CHECK(l0.sepX.size() == 1 && near(l0.sepX[0], (a + b) / 2));
    CHECK(dock::magnify(p, n, 0, 1, 0).sepX.empty());

    // com zoom: item sob o cursor continua sob ele, as células não se sobrepõem e o vão não encolhe
    for (double mx = g.x + 1; mx < g.x + g.w; mx += 2) {
        const auto l = dock::magnify(g, n, mx, 1.8, 1);
        for (size_t i = 1; i < n; i++)
            CHECK(l.cells[i].x >= l.cells[i - 1].x + l.cells[i - 1].size + dock::GAP - 1e-9);
        CHECK(l.cells[2].x - (l.cells[1].x + l.cells[1].size) >= dock::GAP + dock::SEP_W - 1e-9);
        CHECK(l.sepX[0] > l.cells[1].x + l.cells[1].size && l.sepX[0] < l.cells[2].x);
        const int before = dock::hit(g, n, mx, g.y + g.h / 2);
        if (before >= 0)
            CHECK(dock::hit(l, mx, l.cells[before].y + l.cells[before].size / 2) == before);
    }
    // o ponto do separador sob o cursor também fica parado
    const auto ls = dock::magnify(g, n, l0.sepX[0], 1.8, 1);
    CHECK(near(ls.sepX[0], l0.sepX[0]));
}

static void testTwoSeparators() {
    // fixos | abertos | Lixeira
    const size_t n = 6;
    const auto   g = dock::geometry(n, 1920, 1080, 48, 8, {5, 2});
    const auto   p = dock::geometry(n, 1920, 1080, 48, 8);
    CHECK(g.seps.size() == 2 && g.seps[0] == 2 && g.seps[1] == 5); // ordenados
    CHECK(near(g.w, p.w + 2 * dock::SEP_W));
    CHECK(near(dock::cell(g, 5).x - (dock::cell(g, 4).x + dock::cell(g, 4).size), dock::GAP + dock::SEP_W));
    const auto l0 = dock::magnify(g, n, 0, 1, 0);
    CHECK(l0.sepX.size() == 2 && l0.sepX[0] < l0.sepX[1]);
    for (double mx = g.x + 1; mx < g.x + g.w; mx += 2) {
        const auto l = dock::magnify(g, n, mx, 1.8, 1);
        for (size_t i = 1; i < n; i++)
            CHECK(l.cells[i].x >= l.cells[i - 1].x + l.cells[i - 1].size + dock::GAP - 1e-9);
        CHECK(l.sepX[1] > l.cells[4].x + l.cells[4].size && l.sepX[1] < l.cells[5].x);
        const int before = dock::hit(g, n, mx, g.y + g.h / 2);
        if (before >= 0)
            CHECK(dock::hit(l, mx, l.cells[before].y + l.cells[before].size / 2) == before);
    }
    for (auto sx : l0.sepX) // em cima de cada separador, ele fica parado
        CHECK(near(dock::magnify(g, n, sx, 1.8, 1).sepX[sx == l0.sepX[0] ? 0 : 1], sx));
}

static void testMenuSeparators() {
    const dock::Frame       f{dock::Edge::Bottom, 1920, 1080};
    const dock::Box         cell{900, 1000, 60, 60}, panel{600, 990, 700, 80};
    const std::vector<bool> seps = {false, false, true, false};
    const auto              m    = dock::menuGeometry(f, cell, panel, seps);
    CHECK(near(m.h, 3 * dock::MENU_ROW + dock::MENU_SEP + 2 * dock::MENU_PAD));
    CHECK(near(m.y + m.h, panel.y - dock::MENU_GAP)); // acima da dock
    CHECK(near(dock::menuRowY(m, seps, 3), m.y + dock::MENU_PAD + 2 * dock::MENU_ROW + dock::MENU_SEP));
    const double x = m.x + 20;
    CHECK(dock::menuHit(m, seps, x, dock::menuRowY(m, seps, 1) + 1) == 1);
    CHECK(dock::menuHit(m, seps, x, dock::menuRowY(m, seps, 2) + 1) == -1); // separador não é clicável
    CHECK(dock::menuHit(m, seps, x, dock::menuRowY(m, seps, 3) + dock::MENU_ROW - 1) == 3);
    CHECK(dock::menuHit(m, seps, x, m.y + 1) == -1);  // padding
    CHECK(dock::menuHit(m, seps, m.x - 1, m.y + 10) == -2);
    // sem separadores, igual à versão por contagem
    const auto a = dock::menuGeometry(f, cell, panel, std::vector<bool>(3, false));
    const auto b = dock::menuGeometry(f, cell, panel, 3);
    CHECK(near(a.y, b.y) && near(a.h, b.h));
}

static void testSlider() {
    CHECK(near(dock::sliderValue(100, 100, 200, 1, 3), 1));
    CHECK(near(dock::sliderValue(150, 100, 200, 1, 3), 2));
    CHECK(near(dock::sliderValue(50, 100, 200, 1, 3), 1));  // antes do trilho: a ponta
    CHECK(near(dock::sliderValue(300, 100, 200, 1, 3), 3)); // depois: a outra ponta
    CHECK(near(dock::sliderX(2, 100, 200, 1, 3), 150));
    CHECK(near(dock::sliderX(9, 100, 200, 1, 3), 200));
    for (double v = 1.1; v <= 2.5; v += 0.1) // ida e volta
        CHECK(near(dock::sliderValue(dock::sliderX(v, 10, 90, dock::MAG_MIN, dock::MAG_MAX), 10, 90, dock::MAG_MIN, dock::MAG_MAX), v));
    CHECK(near(dock::sliderValue(5, 10, 10, 1, 3), 1)); // trilho vazio
}

static void testSubmenu() {
    const dock::MenuGeom parent{100, 500, dock::MENU_W, 200};
    const auto           s = dock::submenuGeometry(parent, 600, 3, 1920, 1080);
    CHECK(near(s.x, parent.x + parent.w - 2));
    CHECK(near(s.y + dock::MENU_PAD, 600)); // primeira linha na altura da linha pai
    CHECK(near(s.h, 3 * dock::MENU_ROW + 2 * dock::MENU_PAD));
    // sem espaço à direita: abre à esquerda
    const dock::MenuGeom right{1800, 500, dock::MENU_W, 200};
    const auto           s2 = dock::submenuGeometry(right, 600, 3, 1920, 1080);
    CHECK(near(s2.x + s2.w, right.x + 2));
    // perto do fim da tela: sobe
    const auto s3 = dock::submenuGeometry(parent, 1070, 3, 1920, 1080);
    CHECK(s3.y + s3.h <= 1080 - 4 + 1e-9);
}

static void testSnapToEdge() {
    const auto g = dock::geometry(5, 1920, 1080, 48, 8); // margem de 8 px até a borda (y = 1080)
    CHECK(near(dock::snapToEdge(g, 1080, 500), 500));     // longe: não mexe
    CHECK(near(dock::snapToEdge(g, 1080, g.y + 5), g.y + 5)); // em cima da dock: não mexe
    const double s = dock::snapToEdge(g, 1080, 1079);     // na borda da tela
    CHECK(s < g.y + g.h && s > g.y);
    // cai dentro de um ícone (mesmo x)
    const auto c = dock::cell(g, 2);
    CHECK(dock::hit(g, 5, c.x + c.size / 2, s) == 2);
    // e no zoom também
    const auto l = dock::magnify(g, 5, c.x + c.size / 2, 1.8, 1);
    CHECK(dock::hit(l, c.x + c.size / 2, s) == 2);
    CHECK(near(dock::snapToEdge(g, 1080, 1090), 1090)); // além da borda (outro monitor): não mexe
}

int main() {
    testPinnedFirst();
    testGroupsAndSorts();
    testStableOrder();
    testHideRunning();
    testDuplicatePinnedAndEmptyClass();
    testNextWindow();
    testTargetWindow();
    testPins();
    testPinsFile();
    testMoveTo();
    testReorderPins();
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
    testSeparator();
    testTwoSeparators();
    testMenuSeparators();
    testSlider();
    testSubmenu();
    testSnapToEdge();
    testMagnifyHit();
    testBounce();
    testNewlyOpened();
    testDropIndex();
    testDragSlot();
    testDragOutside();
    testPoof();
    testEnter();

    std::printf("%d/%d checagens ok\n", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
