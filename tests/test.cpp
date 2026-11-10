// Testes das partes do hyprtab que não dependem do Hyprland: `make test`.

#include "../src/clients.hpp"
#include "../src/layout.hpp"

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

// ---------- parseClients ----------

static void testClientsTypical() {
    // formato real do `hyprctl clients -j` (resumido): objetos, arrays, números e booleanos
    const std::string js = R"([{
    "address": "0x56eb831649a0",
    "mapped": true,
    "at": [10, 52],
    "workspace": { "id": 1, "name": "1" },
    "class": "google-chrome",
    "title": "Phoronix - Google Chrome",
    "grouped": [],
    "tags": [],
    "pid": 1234
},{
    "address": "0x56eb83076d10",
    "class": "Alacritty",
    "title": "Alacritty",
    "fullscreen": 0
}])";
    const auto c = parseClients(js);
    CHECK(c.size() == 2);
    CHECK(c.at("0x56eb831649a0").first == "google-chrome");
    CHECK(c.at("0x56eb831649a0").second == "Phoronix - Google Chrome");
    CHECK(c.at("0x56eb83076d10").first == "Alacritty");
}

static void testClientsEscapes() {
    const auto c = parseClients(R"([{"address":"0x1","class":"a","title":"diz \"oi\" \\ fim\tTAB\nNL \/ A"}])");
    CHECK(c.at("0x1").second == "diz \"oi\" \\ fim\tTAB\nNL / A");

    // \r, \b, \f são escapes válidos de JSON e não podem virar a letra
    const auto r = parseClients(R"([{"address":"0x1","title":"a\rb\bc\fd"}])");
    CHECK(r.at("0x1").second == "a\rb\bc\fd");
}

static void testClientsUnicode() {
    // UTF-8 cru passa intacto; \u fora do ASCII vira '?'
    const auto c = parseClients("[{\"address\":\"0x1\",\"title\":\"Configurações\",\"class\":\"x\\u00e9\"}]");
    CHECK(c.at("0x1").second == "Configurações");
    CHECK(c.at("0x1").first == "x?");
}

static void testClientsMalformed() {
    // nada disso pode lançar: uma exceção aqui derruba o compositor
    CHECK(parseClients("").empty());
    CHECK(parseClients("[]").empty());
    CHECK(parseClients("  [ ]  ").empty());
    CHECK(parseClients("not json").empty());
    CHECK(parseClients("{}").empty());
    parseClients(R"([{"address":"0x1","title":"abc)");      // truncado no meio da string
    parseClients(R"([{"address":"0x1","title":"\u)");       // \u truncado
    parseClients(R"([{"address":"0x1","title":"\uZZZZ"}])"); // \u inválido
    parseClients(R"([{"address":"0x1","workspace":{"id":)"); // truncado dentro de objeto aninhado
    parseClients(R"([{"address":"0x1","tags":[1,2,)");       // truncado dentro de array
    CHECK(true);
}

static void testClientsSkips() {
    // sem address: ignorado; class/título não-string: ficam vazios
    const auto c = parseClients(R"([{"class":"semaddr"},{"address":"0x2","class":null,"title":42}])");
    CHECK(c.size() == 1);
    CHECK(c.at("0x2").first.empty());
    CHECK(c.at("0x2").second.empty());
}

// ---------- listas e janelas ignoradas ----------

static void testSplitList() {
    CHECK(splitList("").empty());
    CHECK(splitList(" , ,").empty());
    const auto l = splitList(" google-chrome,Alacritty ,, code ");
    CHECK(l.size() == 3);
    CHECK(l[0] == "google-chrome");
    CHECK(l[1] == "Alacritty");
    CHECK(l[2] == "code");
}

static void testIgnored() {
    const auto& d = DEFAULT_IGNORE;
    CHECK(classIgnored("hyprland-share-picker", d));
    CHECK(classIgnored("Hyprland-Share-Picker", d));          // sem diferenciar maiúsculas
    CHECK(classIgnored("xdg-desktop-portal-gtk", d));         // prefixo
    CHECK(classIgnored("org.kde.polkit-kde-authentication-agent-1", d)); // contém
    CHECK(!classIgnored("google-chrome", d));
    CHECK(!classIgnored("Alacritty", d));
    CHECK(!classIgnored("xdg-desktop", d));                   // menor que o prefixo
    CHECK(classIgnored("steam_app_123", {"steam_app_*"}));
    CHECK(classIgnored("foo.bar.Picker", {"*picker"}));       // sufixo
    CHECK(!classIgnored("x", {"", "  "}));                    // padrões vazios não casam com nada
}

// ---------- layout ----------

static void testGeometryCentered() {
    using namespace layout;
    const auto g = geometry(3, 1920, 1080);
    const double cell = ICON + 2 * PAD;
    CHECK(near(g.k, 1));
    CHECK(near(g.w, 3 * cell + 2 * GAP + 2 * BARPAD));
    CHECK(near(g.h, cell + TITLE_H + 2 * BARPAD));
    CHECK(near(g.x, (1920 - g.w) / 2));
    CHECK(near(g.y, (1080 - g.h) / 2));
}

static void testGeometryEmpty() {
    // zero itens é tratado como um (não gera largura negativa)
    const auto a = layout::geometry(0, 1920, 1080), b = layout::geometry(1, 1920, 1080);
    CHECK(near(a.w, b.w));
    CHECK(a.w > 0);
}

static void testGeometryShrinks() {
    // muitas janelas: a barra encolhe para caber com 40px de margem de cada lado
    const auto g = layout::geometry(30, 1920, 1080);
    CHECK(g.k < 1);
    CHECK(near(g.w, 1920 - 80));
    CHECK(near(g.x, 40));
    // a última célula termina exatamente antes do padding direito
    const auto last = layout::cell(g, 29);
    CHECK(near(last.x + last.size + layout::BARPAD * g.k, g.x + g.w));
}

static void testHit() {
    using namespace layout;
    const size_t n = 4;
    const auto   g = geometry(n, 1920, 1080);
    for (size_t i = 0; i < n; i++) {
        const auto c = cell(g, i);
        CHECK(hit(g, n, c.x + c.size / 2, c.y + c.size / 2) == (int)i);
    }
    const auto c0 = cell(g, 0), c1 = cell(g, 1);
    CHECK(hit(g, n, (c0.x + c0.size + c1.x) / 2, c0.y + 10) == -1); // vão entre ícones
    CHECK(hit(g, n, g.x + 2, g.y + g.h - 2) == -1);                  // área do título
    CHECK(hit(g, n, g.x - 1, g.y + g.h / 2) == -2);                  // à esquerda da barra
    CHECK(hit(g, n, g.x + g.w / 2, g.y + g.h + 1) == -2);            // abaixo da barra
    CHECK(hit(g, n, 0, 0) == -2);
}

static void testHitShrunk() {
    using namespace layout;
    const size_t n = 30;
    const auto   g = geometry(n, 1280, 720);
    const auto   c = cell(g, 17);
    CHECK(hit(g, n, c.x + c.size / 2, c.y + c.size / 2) == 17);
}

int main() {
    testClientsTypical();
    testClientsEscapes();
    testClientsUnicode();
    testClientsMalformed();
    testClientsSkips();
    testSplitList();
    testIgnored();
    testGeometryCentered();
    testGeometryEmpty();
    testGeometryShrinks();
    testHit();
    testHitShrunk();

    std::printf("%d/%d checagens ok\n", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
