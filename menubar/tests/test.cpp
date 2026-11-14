// Testes do modelo do hyprmenu (sem dependência do Hyprland nem do D-Bus): `make test`.

#include "../src/model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

// 7 px por caractere; negrito 8
static const mb::Measure MEASURE = [](const std::string& s, bool bold) { return (double)s.size() * (bold ? 8 : 7); };

static mb::Node item(const std::string& label, const std::string& shortcut = "") {
    mb::Node n;
    n.label    = label;
    n.shortcut = shortcut;
    return n;
}

static mb::Node sep() {
    mb::Node n;
    n.separator = true;
    return n;
}

static void testMnemonic() {
    CHECK(mb::stripMnemonic("_Arquivo") == "Arquivo");
    CHECK(mb::stripMnemonic("Salvar _como...") == "Salvar como...");
    CHECK(mb::stripMnemonic("a__b") == "a_b");
    CHECK(mb::stripMnemonic("sem") == "sem");
    CHECK(mb::stripMnemonic("_") == "");
}

static void testShortcut() {
    CHECK(mb::formatShortcut({{"Control", "Shift", "s"}}) == "Ctrl+Shift+S");
    CHECK(mb::formatShortcut({{"Control", "plus"}}) == "Ctrl++");
    CHECK(mb::formatShortcut({{"F11"}}) == "F11");
    CHECK(mb::formatShortcut({{"Control", "q"}, {"Alt", "F4"}}) == "Ctrl+Q, Alt+F4");
    CHECK(mb::formatShortcut({{}}) == "");
    CHECK(mb::formatShortcut({}) == "");
}

static void testAt() {
    mb::Node root;
    root.children = {item("a"), item("b")};
    root.children[1].children = {item("b0"), item("b1")};
    CHECK(mb::at(root, {}) == &root);
    CHECK(mb::at(root, {1, 1})->label == "b1");
    CHECK(mb::at(root, {2}) == nullptr);
    CHECK(mb::at(root, {0, 0}) == nullptr);
}

static void testNextSelectable() {
    mb::Node m;
    m.children = {item("a"), sep(), item("c"), item("d")};
    m.children[3].enabled = false;
    CHECK(mb::nextSelectable(m, -1, 1) == 0);
    CHECK(mb::nextSelectable(m, -1, -1) == 2); // pula o desabilitado
    CHECK(mb::nextSelectable(m, 0, 1) == 2);   // pula o separador
    CHECK(mb::nextSelectable(m, 2, 1) == 0);   // dá a volta
    CHECK(mb::nextSelectable(m, 0, -1) == 2);
    mb::Node empty;
    CHECK(mb::nextSelectable(empty, -1, 1) == -1);
    mb::Node none;
    none.children = {sep()};
    CHECK(mb::nextSelectable(none, -1, 1) == -1);
}

static void testFieldCodes() {
    CHECK(mb::stripFieldCodes("code %F") == "code");
    CHECK(mb::stripFieldCodes("a %% b") == "a % b");
    CHECK(mb::stripFieldCodes("/usr/bin/google-chrome-stable %U") == "/usr/bin/google-chrome-stable");
}

static void testTitles() {
    const auto t = mb::layoutTitles({0, 40, 30});
    CHECK(t.size() == 3);
    CHECK(near(t[0].x, mb::BAR_PADX) && near(t[0].w, mb::LOGO_W));
    CHECK(near(t[1].x, mb::BAR_PADX + mb::LOGO_W) && near(t[1].w, 40 + 2 * mb::TITLE_PADX));
    CHECK(near(t[2].x, t[1].x + t[1].w));
    CHECK(mb::hitTitle(t, mb::BAR_PADX) == 0);
    CHECK(mb::hitTitle(t, t[1].x + 1) == 1);
    CHECK(mb::hitTitle(t, t[2].x + t[2].w + 1) == -1);
    CHECK(mb::hitTitle(t, 0) == -1);
}

static void testDropdown() {
    mb::Node m;
    m.children = {item("Abrir", "Ctrl+O"), sep(), item("Sair")};
    const double w = mb::dropdownWidth(m, MEASURE);
    CHECK(near(w, std::max(mb::MENU_MIN_W, 2 * mb::MENU_PADX + 2 * mb::ROW_PADX + mb::CHECK_W + 35 + mb::SHORT_GAP + 42)));

    const mb::Title t{100, 50};
    const auto      d = mb::dropdown(m, MEASURE, t, 28, 1920, 1080);
    CHECK(near(d.box.x, 100) && near(d.box.y, 28));
    CHECK(near(d.box.h, 2 * mb::MENU_PADY + 2 * mb::ROW_H + mb::SEP_H));
    CHECK(d.rows.size() == 3);
    CHECK(near(d.rows[1].h, mb::SEP_H) && near(d.rows[2].y, d.rows[1].y + mb::SEP_H));
    CHECK(mb::hitRow(d, d.rows[2].x + 1, d.rows[2].y + 1) == 2);
    CHECK(mb::hitRow(d, d.box.x + 1, d.box.y + 1) == -1); // padding de cima
    CHECK(mb::hitRow(d, d.box.x - 1, d.box.y + 10) == -2);

    // perto da borda direita: o menu volta para dentro
    const auto e = mb::dropdown(m, MEASURE, {1900, 40}, 28, 1920, 1080);
    CHECK(near(e.box.x + e.box.w, 1920 - mb::SCREEN_MARGIN));
}

static void testWidthClamp() {
    mb::Node m;
    m.children = {item(std::string(200, 'x'))}; // texto comprido é cortado com "…": o menu para em LABEL_MAX_W
    CHECK(near(mb::dropdownWidth(m, MEASURE), 2 * mb::MENU_PADX + 2 * mb::ROW_PADX + mb::CHECK_W + mb::LABEL_MAX_W));
    m.maxLabelW = 100; // limite do próprio menu (o de volume)
    CHECK(near(mb::dropdownWidth(m, MEASURE), mb::MENU_MIN_W));
    m.maxLabelW = 0;
    m.children = {item("a", std::string(200, 'x'))}; // atalho enorme: MENU_MAX_W
    CHECK(near(mb::dropdownWidth(m, MEASURE), mb::MENU_MAX_W));
    m.children[0].submenu = true;
    m.children[0].label   = "a";
    m.children[0].shortcut.clear();
    CHECK(near(mb::dropdownWidth(m, MEASURE), mb::MENU_MIN_W));
}

static void testSubmenu() {
    mb::Node m;
    m.children = {item("a"), item("b")};
    const auto parent = mb::dropdown(m, MEASURE, {100, 40}, 28, 1920, 1080);
    const auto s      = mb::submenu(m, MEASURE, parent, 1, 1920, 1080);
    CHECK(near(s.box.x, parent.box.x + parent.box.w - 2));
    CHECK(near(s.rows[0].y, parent.rows[1].y)); // a primeira linha fica na altura da linha pai

    // sem espaço à direita: abre para a esquerda
    const auto p2 = mb::dropdown(m, MEASURE, {1700, 40}, 28, 1920, 1080);
    const auto s2 = mb::submenu(m, MEASURE, p2, 0, 1920, 1080);
    CHECK(near(s2.box.x + s2.box.w, p2.box.x + 2));

    // embaixo da tela: sobe
    const auto s3 = mb::submenu(m, MEASURE, parent, 1, 1920, 100);
    CHECK(s3.box.y + s3.box.h <= 100 - mb::SCREEN_MARGIN + 1e-9);
}

static void testClock() {
    CHECK(mb::clockText(3, 12, 10, 14, 5) == "qua 12 nov  14:05");
    CHECK(mb::clockText(0, 1, 0, 0, 0) == "dom 1 jan  00:00");
}

static void testJson() {
    mb::Json    j;
    std::string err;
    CHECK(mb::parseJson(R"({"a": [1, true, null, "xé\n"], "b": {"c": -2.5}})", j, err));
    CHECK(j.get("a") && j.get("a")->array.size() == 4);
    CHECK(j.get("a")->array[1].boolean);
    CHECK(j.get("a")->array[3].string == "x\xc3\xa9\n");
    CHECK(near(j.get("b")->get("c")->number, -2.5));
    CHECK(j.get("z") == nullptr);
    CHECK(mb::parseJson(R"("😀")", j, err) && j.string == "\xf0\x9f\x98\x80");

    CHECK(!mb::parseJson("{\"a\": 1,\n \"b\" 2}", j, err));
    CHECK(err.starts_with("linha 2"));
    CHECK(!mb::parseJson("[1, 2", j, err));
    CHECK(!mb::parseJson("{} x", j, err));
    CHECK(!mb::parseJson("", j, err));
}

static void testParseKeys() {
    auto k = mb::parseKeys("Ctrl+Shift+P");
    CHECK(k.size() == 1 && k[0].mods == "CTRL SHIFT" && k[0].key == "p");
    k = mb::parseKeys("Ctrl+K Ctrl+O");
    CHECK(k.size() == 2 && k[1].mods == "CTRL" && k[1].key == "o");
    k = mb::parseKeys("Ctrl+K S");
    CHECK(k.size() == 2 && k[1].mods.empty() && k[1].key == "s");
    CHECK(mb::parseKeys("Ctrl+,")[0].key == "comma");
    CHECK(mb::parseKeys("Ctrl++")[0].key == "plus");
    CHECK(mb::parseKeys("Ctrl+=")[0].key == "equal");
    CHECK(mb::parseKeys("F5")[0].key == "F5" && mb::parseKeys("F5")[0].mods.empty());
    CHECK(mb::parseKeys("Shift+F12")[0].key == "F12");
    CHECK(mb::parseKeys("Alt+Left")[0].key == "Left");
    CHECK(mb::parseKeys("Ctrl+Enter")[0].key == "Return");
    CHECK(mb::parseKeys("Shift+Del")[0].key == "Delete");
    CHECK(mb::parseKeys("Ctrl+code:49")[0].key == "code:49");
    CHECK(mb::parseKeys("Ctrl+1")[0].key == "1");
    CHECK(mb::parseKeys("Super+Alt+x")[0].mods == "SUPER ALT");
    CHECK(mb::parseKeys("Hyper+X").empty());
    CHECK(mb::parseKeys("Ctrl+F99").empty());
    CHECK(mb::parseKeys("Ctrl+code:").empty());
    CHECK(mb::parseKeys("").empty());
}

static void testAppMap() {
    const std::string text = R"({
        "name": "Editor",
        "match": ["Code", "com.microsoft.VSCode"],
        "menus": [
            {"label": "Arquivo", "items": [
                {"label": "Salvar", "keys": "Ctrl+S"},
                "-",
                {"label": "Terminal", "keys": "Ctrl+`", "send": "Ctrl+code:49"},
                {"label": "Salvar Tudo"},
                {"label": "Preferências", "items": [ {"label": "Config", "keys": "Ctrl+,"} ]}
            ]}
        ]
    })";
    mb::AppMap  m;
    std::string err;
    CHECK(mb::parseAppMap(text, m, err));
    CHECK(m.name == "Editor" && m.match.size() == 2 && m.match[0] == "code");
    CHECK(m.menus.children.size() == 1);
    const auto& f = m.menus.children[0];
    CHECK(f.label == "Arquivo" && f.submenu && f.children.size() == 5);
    CHECK(f.children[0].shortcut == "Ctrl+S" && f.children[0].keys == "Ctrl+S" && f.children[0].enabled);
    CHECK(f.children[1].separator);
    CHECK(f.children[2].shortcut == "Ctrl+`" && f.children[2].keys == "Ctrl+code:49");
    CHECK(!f.children[3].enabled); // sem atalho: desabilitado
    CHECK(f.children[4].submenu && f.children[4].children[0].keys == "Ctrl+,");
    CHECK(f.children[0].id != f.children[2].id);

    const std::vector<mb::AppMap> maps = {m};
    CHECK(mb::findAppMap(maps, {"", "COM.MICROSOFT.VSCODE"}) == &maps[0]);
    CHECK(mb::findAppMap(maps, {"firefox"}) == nullptr);

    CHECK(!mb::parseAppMap(R"({"menus": []})", m, err) && err.contains("match"));
    CHECK(!mb::parseAppMap(R"({"match": ["a"], "menus": [{"label": "X", "items": [{"label": "Y", "keys": "Ctrl+Nada"}]}]})", m, err));
    CHECK(err.contains("Ctrl+Nada"));
    CHECK(!mb::parseAppMap(R"({"match": ["a"], "menus": [{"label": "X", "items": []}]})", m, err));
    CHECK(!mb::parseAppMap(R"({"match": ["a"], "menus": [{"items": []}]})", m, err) && err.contains("label"));
}

// todos os mapas que vêm com o plugin são válidos e cada classe aparece em um só mapa
static void testBundledMaps() {
    std::vector<mb::AppMap> maps;
    std::error_code         ec;
    for (auto& e : std::filesystem::directory_iterator("apps", ec)) {
        if (e.path().extension() != ".json")
            continue;
        std::ifstream in(e.path());
        std::string   text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        mb::AppMap    m;
        std::string   err;
        const bool    ok = mb::parseAppMap(text, m, err);
        if (!ok)
            std::printf("  %s: %s\n", e.path().c_str(), err.c_str());
        CHECK(ok);
        maps.push_back(std::move(m));
    }
    CHECK(maps.size() >= 4);
    for (size_t a = 0; a < maps.size(); a++)
        for (size_t b = a + 1; b < maps.size(); b++)
            for (auto& c : maps[a].match)
                CHECK(std::find(maps[b].match.begin(), maps[b].match.end(), c) == maps[b].match.end());
}

static void testVolume() {
    mb::Volume v;
    CHECK(mb::parseVolume("Volume: 0.40\n", v) && near(v.vol, 0.40) && !v.muted);
    CHECK(mb::parseVolume("Volume: 1.25 [MUTED]\n", v) && near(v.vol, 1.25) && v.muted);
    CHECK(!mb::parseVolume("Translate ID failed", v));
    CHECK(!mb::parseVolume("Volume: x", v));

    const std::string status = "PipeWire 'pipewire-0' [1.6.2]\n"
                               " └─ Clients:\n"
                               "        33. WirePlumber                         [1.6.2]\n"
                               "\n"
                               "Audio\n"
                               " ├─ Devices:\n"
                               " │      52. Ryzen HD Audio Controller (PRIME B450M-A Motherboard) [alsa]\n"
                               " │  \n"
                               " ├─ Sinks:\n"
                               " │  *   51. Ryzen HD Audio Controller (PRIME B450M-A Motherboard) Estéreo digital (IEC958) [vol: 0.40]\n"
                               " │      57. Raven HDMI/DP Audio Controller (PRIME) Estéreo digital (HDMI) [vol: 0.40 MUTED]\n"
                               " │  \n"
                               " ├─ Sources:\n"
                               " │      61. Microfone [vol: 1.00]\n"
                               " └─ Streams:\n"
                               "\n"
                               "Video\n"
                               " ├─ Sinks:\n"
                               " │      70. Câmera virtual\n";
    const auto sinks = mb::parseSinks(status);
    CHECK(sinks.size() == 2);
    if (sinks.size() == 2) {
        CHECK(sinks[0].id == 51 && sinks[0].def && sinks[0].name == "Ryzen HD Audio Controller (PRIME B450M-A Motherboard) Estéreo digital (IEC958)");
        CHECK(sinks[1].id == 57 && !sinks[1].def);
        CHECK(sinks[0].label == "Estéreo digital (IEC958)");
        // dispositivo 57 não está em Devices no texto de teste: cai no nome encurtado
        CHECK(sinks[1].label == "Raven HDMI/DP Audio Controller Estéreo digital (HDMI)");
    }
    // dois dispositivos com o mesmo perfil: os dois mostram o nome (encurtado), não "Estéreo analógico" duas vezes
    const auto twin = mb::parseSinks("Audio\n"
                                     " ├─ Devices:\n"
                                     " │      40. Fone A [alsa]\n"
                                     " │      41. Fone B [alsa]\n"
                                     " │      42. HDMI Controller [alsa]\n"
                                     " ├─ Sinks:\n"
                                     " │  *   50. Fone A Estéreo analógico [vol: 0.40]\n"
                                     " │      51. Fone B Estéreo analógico [vol: 0.40]\n"
                                     " │      52. HDMI Controller Estéreo digital (HDMI) [vol: 0.40]\n");
    CHECK(twin.size() == 3);
    if (twin.size() == 3) {
        CHECK(twin[0].label == "Fone A Estéreo analógico");
        CHECK(twin[1].label == "Fone B Estéreo analógico");
        CHECK(twin[2].label == "Estéreo digital (HDMI)");
    }
    CHECK(mb::shortSinkName("Ryzen HD Audio Controller (PRIME B450M-A Motherboard) Estéreo digital (IEC958)") == "Ryzen HD Audio Controller Estéreo digital (IEC958)");
    CHECK(mb::shortSinkName("Fone (USB)") == "Fone (USB)");
    CHECK(mb::parseSinks("").empty());

    // reticências: 7 px por byte no MEASURE dos testes ("…" tem 3 bytes = 21 px)
    CHECK(mb::ellipsize("curto", 100, false, MEASURE) == "curto");
    CHECK(mb::ellipsize("abcdefghij", 50, false, MEASURE) == "abcd…");   // 4*7 + 21 = 49
    CHECK(mb::ellipsize("abcéfghij", 50, false, MEASURE) == "abc…");    // não corta no meio do "é"
    CHECK(mb::ellipsize("abc defghij", 50, false, MEASURE) == "abc…");  // sem espaço antes do "…"

    CHECK(mb::volumeLevel(0.5, true) == 0);
    CHECK(mb::volumeLevel(0, false) == 0);
    CHECK(mb::volumeLevel(0.2, false) == 1);
    CHECK(mb::volumeLevel(0.5, false) == 2);
    CHECK(mb::volumeLevel(1.0, false) == 3);

    // ícone à esquerda do relógio; sem relógio, encostado na margem
    const auto t = mb::volumeTitle(1000, 100);
    CHECK(near(t.x + t.w, 1000 - mb::CLOCK_PADX - 100 - mb::STATUS_GAP));
    CHECK(near(mb::volumeTitle(1000, 0).x + mb::VOLUME_W, 1000 - mb::BAR_PADX));

    // menu com slider: linha mais alta, largura mínima maior, cabeçalho não selecionável
    mb::Node menu, sl, hd;
    sl.slider = true;
    hd.header = true;
    hd.label  = "Saída";
    menu.children = {sl, sep(), hd, item("A")};
    const auto d  = mb::dropdown(menu, MEASURE, {900, 30}, 28, 1000, 800);
    CHECK(near(d.box.w, mb::SLIDER_MIN_W));
    CHECK(near(d.rows[0].h, mb::SLIDER_H));
    CHECK(d.box.x + d.box.w <= 1000 - mb::SCREEN_MARGIN + 1e-9);
    CHECK(!mb::selectable(hd));
    CHECK(mb::nextSelectable(menu, 0, 1) == 3);
    const double pctW = MEASURE("100%", false);
    const auto   tr   = mb::sliderTrack(d.rows[0], pctW);
    CHECK(near(mb::sliderValueAt(d.rows[0], pctW, tr.x), 0) && near(mb::sliderValueAt(d.rows[0], pctW, tr.x + tr.w), 1));
    CHECK(near(mb::sliderValueAt(d.rows[0], pctW, tr.x + tr.w / 2), 0.5));
    CHECK(near(mb::sliderValueAt(d.rows[0], pctW, -50), 0) && near(mb::sliderValueAt(d.rows[0], pctW, 5000), 1));
    // em 100% o botão (inteiro) termina antes do texto "100%", que fica encostado à direita
    const auto& row = d.rows[0];
    CHECK(tr.x + tr.w + mb::SLIDER_KNOB_R + mb::SLIDER_GAP <= row.x + row.w - mb::ROW_PADX - pctW + 1e-9);
    CHECK(tr.x - mb::SLIDER_KNOB_R >= row.x + mb::ROW_PADX - 1e-9);
}

int main() {
    testMnemonic();
    testShortcut();
    testAt();
    testNextSelectable();
    testFieldCodes();
    testTitles();
    testDropdown();
    testWidthClamp();
    testSubmenu();
    testClock();
    testJson();
    testParseKeys();
    testAppMap();
    testBundledMaps();
    testVolume();

    std::printf("%d/%d checagens ok\n", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
