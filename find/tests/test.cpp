// Testes do modelo do hyprfind (sem dependência do Hyprland): `make test`.

#include "../src/files.hpp"
#include "../src/model.hpp"

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

static bool near(std::optional<double> a, double b) {
    return a && std::abs(*a - b) < 1e-9;
}

static void testText() {
    CHECK(hf::fold("Configurações") == "configuracoes");
    CHECK(hf::fold("ÁÉÍÓÚ àèìòù ÂÊÔ ãõ ÇÑ ü") == "aeiou aeiou aeo ao cn u");
    CHECK(hf::fold("Straße") == "straße"); // ß não vira outra letra
    std::string s = "pão";
    hf::popChar(s);
    CHECK(s == "pã");
    hf::popChar(s);
    CHECK(s == "p");
    hf::popChar(s);
    hf::popChar(s);
    CHECK(s.empty());
    s = "abrir  o arquivo  ";
    hf::popWord(s);
    CHECK(s == "abrir  o ");
    CHECK(hf::shellQuote("it's") == "'it'\\''s'");
    CHECK((hf::words("  a b\tc  ") == std::vector<std::string>{"a", "b", "c"}));
}

static void testFuzzy() {
    CHECK(hf::fuzzy("fire", "firefox") > hf::fuzzy("fox", "firefox"));                  // começo > meio
    CHECK(hf::fuzzy("code", "visual studio code") > hf::fuzzy("vsc", "visual studio code")); // trecho > espalhado
    CHECK(hf::fuzzy("vsc", "visual studio code") > 0);
    CHECK(hf::fuzzy("xyz", "visual studio code") == -1);
    CHECK(hf::fuzzy("ff", "firefox") > 0);
    CHECK(hf::fuzzy("term", "terminal") > hf::fuzzy("term", "gnome terminal")); // texto mais curto, no começo
    CHECK(hf::fuzzy("studio", "visual studio code") > hf::fuzzy("tudio", "visual studio code")); // começo de palavra
    CHECK(hf::fuzzy("", "abc") == 0);
    CHECK(hf::fuzzy("abcd", "abc") == -1);
    CHECK(hf::fuzzy("fire", "configuracoes") == -1);                 // letras soltas no meio: não
    CHECK(hf::fuzzy("hyprfind", "redshift hyprland gammastep indicator") == -1);
    CHECK(hf::fuzzy("gimp", "gnu image manipulation program") > 0);  // iniciais de palavras: sim
}

static void testCalc() {
    CHECK(near(hf::calc("2+3*4"), 14));
    CHECK(near(hf::calc("(2+3)*4"), 20));
    CHECK(near(hf::calc("2^10"), 1024));
    CHECK(near(hf::calc("2^-1"), 0.5));
    CHECK(near(hf::calc("-2^2"), -4));
    CHECK(near(hf::calc("1,5 + 1,5"), 3));
    CHECK(near(hf::calc("10 / 4"), 2.5));
    CHECK(near(hf::calc("3x4"), 12));
    CHECK(near(hf::calc("3 × 4 ÷ 2"), 6));
    CHECK(near(hf::calc("200*15%"), 30));
    CHECK(near(hf::calc("sqrt(16)+1"), 5));
    CHECK(near(hf::calc("raiz 9"), 3));
    CHECK(near(hf::calc("2*pi"), 2 * M_PI));
    CHECK(!hf::calc("42"));       // número sozinho não é conta
    CHECK(!hf::calc("-5"));
    CHECK(!hf::calc("pi"));       // nem constante sozinha
    CHECK(!hf::calc("e"));
    CHECK(!hf::calc("firefox"));
    CHECK(!hf::calc("1/0"));      // infinito
    CHECK(!hf::calc("2+"));
    CHECK(!hf::calc("(2+3"));
    CHECK(!hf::calc("x+1"));
    CHECK(!hf::calc(""));

    CHECK(hf::formatNumber(14, false) == "14");
    CHECK(hf::formatNumber(2.5, true) == "2,5");
    CHECK(hf::formatNumber(1.0 / 3, false) == "0.3333333333");
    CHECK(hf::formatNumber(-0.0, false) == "0");
    CHECK(hf::formatNumber(0.1 + 0.2, false) == "0.3");
    CHECK(hf::formatNumber(1e20, false) == "1e+20");
    CHECK(hf::usesComma("1,5+1") && !hf::usesComma("1.5+1"));
}

static const char* FIREFOX = R"([Desktop Entry]
Version=1.0
Name=Firefox Web Browser
Name[pt_BR]=Navegador Firefox
GenericName=Web Browser
GenericName[pt_BR]=Navegador web
Keywords=Internet;WWW;Browser;
Keywords[pt_BR]=internet;navegador;
Exec=firefox %u
Icon=firefox
Terminal=false
Type=Application

[Desktop Action new-window]
Name=Open a New Window
Exec=firefox --new-window %u
)";

static void testDesktop() {
    const auto langs = hf::localeLangs("pt_BR.UTF-8");
    CHECK((langs == std::vector<std::string>{"pt_BR", "pt"}));
    CHECK(hf::localeLangs("C").empty());
    CHECK((hf::localeLangs("de_DE@euro") == std::vector<std::string>{"de_DE", "de"}));

    CHECK(hf::stripFieldCodes("firefox %u") == "firefox");
    CHECK(hf::stripFieldCodes("code --open \"%F\" --x") == "code --open --x");
    CHECK(hf::stripFieldCodes("echo 100%%") == "echo 100%");

    auto a = hf::parseDesktop("firefox", FIREFOX, langs, "Hyprland");
    CHECK(a && a->name == "Navegador Firefox");
    CHECK(a && a->exec == "firefox"); // o Exec da Action não sobrescreve
    CHECK(a && a->icon == "firefox");
    CHECK(a && a->extra.find("Web Browser") != std::string::npos); // o nome em inglês também casa
    CHECK(a && !a->terminal);

    auto en = hf::parseDesktop("firefox", FIREFOX, {}, "Hyprland");
    CHECK(en && en->name == "Firefox Web Browser");

    auto with = [](const std::string& extra) { return std::string("[Desktop Entry]\nType=Application\nName=X\nExec=x\n") + extra; };
    CHECK(!hf::parseDesktop("x", with("NoDisplay=true\n"), {}, "Hyprland"));
    CHECK(!hf::parseDesktop("x", with("Hidden=true\n"), {}, "Hyprland"));
    CHECK(!hf::parseDesktop("x", with("OnlyShowIn=GNOME;KDE;\n"), {}, "Hyprland"));
    CHECK(hf::parseDesktop("x", with("OnlyShowIn=GNOME;\n"), {}, "ubuntu:GNOME"));
    CHECK(!hf::parseDesktop("x", with("NotShowIn=Hyprland;\n"), {}, "Hyprland"));
    CHECK(hf::parseDesktop("x", with("Terminal=true\n"), {}, "Hyprland")->terminal);
    CHECK(!hf::parseDesktop("x", "[Desktop Entry]\nType=Link\nName=X\nURL=y\n", {}, "Hyprland"));
    CHECK(!hf::parseDesktop("x", "[Desktop Entry]\nType=Application\nName=X\n", {}, "Hyprland")); // sem Exec

    // a pasta do usuário vem antes e esconde a do sistema (Hidden=true); subpasta vira prefixo do id
    namespace fs   = std::filesystem;
    const auto tmp = fs::temp_directory_path() / ("hyprfind-test-" + std::to_string(getpid()));
    fs::create_directories(tmp / "user");
    fs::create_directories(tmp / "sys/kde");
    std::ofstream(tmp / "user/foo.desktop") << "[Desktop Entry]\nType=Application\nName=Foo\nExec=foo\nHidden=true\n";
    std::ofstream(tmp / "sys/foo.desktop") << "[Desktop Entry]\nType=Application\nName=Foo\nExec=foo\n";
    std::ofstream(tmp / "sys/bar.desktop") << "[Desktop Entry]\nType=Application\nName=Bar\nExec=bar\n";
    std::ofstream(tmp / "sys/kde/baz.desktop") << "[Desktop Entry]\nType=Application\nName=Abc\nExec=baz\n";
    std::ofstream(tmp / "sys/gone.desktop") << "[Desktop Entry]\nType=Application\nName=Gone\nExec=gone\nTryExec=/nao/existe/gone\n";
    const auto apps = hf::loadApps({(tmp / "user").string(), (tmp / "sys").string()}, {}, "Hyprland");
    CHECK(apps.size() == 2);
    CHECK(apps.size() == 2 && apps[0].id == "kde-baz" && apps[1].id == "bar"); // em ordem de nome

    // varredura de arquivos: sem ocultos nem node_modules, pastas com '/'
    fs::create_directories(tmp / "home/docs/sub");
    fs::create_directories(tmp / "home/.cache");
    fs::create_directories(tmp / "home/proj/node_modules/lib");
    std::ofstream(tmp / "home/docs/relatorio.pdf") << "x";
    std::ofstream(tmp / "home/docs/sub/notas.txt") << "x";
    std::ofstream(tmp / "home/.cache/lixo") << "x";
    std::ofstream(tmp / "home/proj/node_modules/lib/a.js") << "x";
    std::atomic<bool> stop{false};
    auto              list = hf::walkFiles((tmp / "home").string(), 1000, stop);
    std::sort(list.begin(), list.end());
    CHECK((list == std::vector<std::string>{"docs/", "docs/relatorio.pdf", "docs/sub/", "docs/sub/notas.txt", "proj/"}));
    CHECK(hf::walkFiles((tmp / "home").string(), 2, stop).size() == 2);

    // fzf de verdade, se houver nesta máquina
    if (const auto fzf = hf::findFzf(getenv("HOME") ? getenv("HOME") : ""); !fzf.empty()) {
        std::string blob;
        for (auto& s : list)
            blob += s + "\n";
        auto r = hf::fzfFilter(fzf, "notas", blob, 5, nullptr);
        CHECK(r && !r->empty() && r->front() == "docs/sub/notas.txt");
        auto none = hf::fzfFilter(fzf, "zzzzqqq", blob, 5, nullptr);
        CHECK(none && none->empty());
        auto bad = hf::fzfFilter(fzf, "x", blob, 5, nullptr); // ainda funciona com consulta sem resultado ou com
        CHECK(bad.has_value());
        std::printf("fzf: %s\n", fzf.c_str());
    } else
        std::printf("fzf: não encontrado (testes do fzf pulados)\n");
    CHECK(!hf::fzfFilter("/nao/existe/fzf", "x", "a\n", 5, nullptr));

    fs::remove_all(tmp);
}

static void testSearch() {
    std::vector<hf::App> apps = {
        {"firefox", "Navegador Firefox", "Web Browser internet", "firefox", "firefox", "", false},
        {"code", "Visual Studio Code", "editor text", "code", "code", "", false},
        {"org.gnome.Settings", "Configurações", "Settings preferences", "gnome-control-center", "", "", false},
        {"alacritty", "Alacritty", "Terminal", "alacritty", "", "", false},
        {"foot", "Foot", "Terminal", "foot", "", "", false},
    };
    std::vector<hf::Window> wins = {{"0x1", "firefox", "GitHub - hyprtab", "Navegador Firefox"}, {"0x2", "Alacritty", "~/DEV", "Alacritty"}};
    const std::unordered_map<std::string, int> noUsage;

    CHECK(hf::search("", apps, wins, {}, "/h", noUsage).empty());
    CHECK(hf::search("   ", apps, wins, {}, "/h", noUsage).empty());

    auto r = hf::search("config", apps, wins, {}, "/h", noUsage);
    CHECK(!r.empty() && r[0].kind == hf::Kind::App && r[0].key == "org.gnome.Settings");

    r = hf::search("settings", apps, wins, {}, "/h", noUsage); // nome em inglês, valendo menos, ainda acha
    CHECK(!r.empty() && r[0].key == "org.gnome.Settings");

    r = hf::search("vs code", apps, wins, {}, "/h", noUsage); // todas as palavras precisam casar
    CHECK(r.size() == 1 && r[0].key == "code");

    // "terminal" casa os dois só pelo GenericName; o mais usado sobe
    r = hf::search("terminal", apps, wins, {}, "/h", {{"foot", 3}});
    CHECK(r.size() >= 2 && r[0].key == "foot" && r[1].key == "alacritty");

    // seções na ordem: conta, apps, janelas, arquivos
    r = hf::search("fire", apps, wins, {"docs/firewall.txt", "fire/"}, "/h", noUsage);
    CHECK(r.size() == 4);
    CHECK(r.size() == 4 && r[0].kind == hf::Kind::App && r[1].kind == hf::Kind::Window && r[1].key == "0x1" && r[2].kind == hf::Kind::File);
    CHECK(r.size() == 4 && r[2].title == "firewall.txt" && r[2].subtitle == "~/docs" && r[2].key == "/h/docs/firewall.txt" && r[2].icon == "text-x-generic");
    CHECK(r.size() == 4 && r[3].title == "fire" && r[3].subtitle == "~" && r[3].icon == "folder" && r[3].key == "/h/fire");

    r = hf::search("2+2", apps, wins, {}, "/h", noUsage);
    CHECK(!r.empty() && r[0].kind == hf::Kind::Calc && r[0].title == "= 4" && r[0].key == "4");
    r = hf::search("1,5*2", apps, wins, {}, "/h", noUsage);
    CHECK(!r.empty() && r[0].key == "3");
    r = hf::search("1,25*2", apps, wins, {}, "/h", noUsage);
    CHECK(!r.empty() && r[0].key == "2,5");

    hf::Limits lim;
    lim.apps = 1;
    CHECK(hf::search("o", apps, {}, {}, "/h", noUsage, lim).size() == 1);

    // ordem interna dos arquivos: nome do arquivo antes do caminho, mais raso antes
    std::vector<std::string> files = {"proj/notas/readme.md", "notas.txt", "a/b/c/notas.txt", "outra/coisa.txt"};
    auto                     f     = hf::rankFiles("notas", files, 10);
    CHECK(f.size() == 3 && f[0] == "notas.txt" && f[1] == "a/b/c/notas.txt" && f[2] == "proj/notas/readme.md");
    CHECK(hf::rankFiles("notas", files, 1).size() == 1);
    CHECK(hf::rankFiles("", files, 10).empty());
    CHECK(hf::rankFiles("coisa txt", files, 10) == std::vector<std::string>{"outra/coisa.txt"});
}

static void testRows() {
    std::vector<hf::Result> res(5);
    res[0].kind = hf::Kind::Calc;
    res[1].kind = res[2].kind = hf::Kind::App;
    res[3].kind = res[4].kind = hf::Kind::File;
    const auto rows = hf::layoutRows(res, 40, 20);
    CHECK(rows.size() == 8); // 3 títulos + 5 linhas
    CHECK(rows[0].result == -1 && rows[0].header == "Calculadora");
    CHECK(rows[1].result == 0 && rows[1].y == 20);
    CHECK(rows[2].result == -1 && rows[2].y == 60 && rows[2].header == "Aplicativos");
    CHECK(rows[3].result == 1 && rows[3].y == 80);
    CHECK(hf::rowsHeight(rows) == 3 * 20 + 5 * 40);

    // janela de 100 (títulos: 0, 60, 160; linhas: 20, 80, 120, 180, 220)
    CHECK(hf::scrollFor(rows, 0, 100, 0) == 0);
    CHECK(hf::scrollFor(rows, 3, 100, 0) == 120);  // linha 180..220, título em 160: os dois aparecem
    CHECK(hf::scrollFor(rows, 4, 100, 0) == 160);  // última: 220..260 -> 160 (fundo)
    CHECK(hf::scrollFor(rows, 0, 100, 160) == 0);  // voltar ao topo mostra o título
    CHECK(hf::scrollFor(rows, 2, 100, 100) == 100); // já visível: não mexe
    CHECK(hf::scrollFor(rows, 4, 1000, 50) == 0);  // tudo cabe

    CHECK(hf::hitRow(rows, 5, 0, 100) == -1);  // título
    CHECK(hf::hitRow(rows, 25, 0, 100) == 0);
    CHECK(hf::hitRow(rows, 25, 60, 100) == 1); // rolado: 85 -> linha do 1
    CHECK(hf::hitRow(rows, 120, 0, 100) == -1); // fora da área visível
}

static void testUsage() {
    const auto u = hf::parseUsage("3\tfirefox\n10\tcode\nlixo\n0\tzero\n");
    CHECK(u.size() == 2 && u.at("firefox") == 3 && u.at("code") == 10);
    CHECK(hf::parseUsage(hf::formatUsage(u)) == u);
}

int main() {
    testText();
    testFuzzy();
    testCalc();
    testDesktop();
    testSearch();
    testRows();
    testUsage();
    std::printf("%d verificações, %d falhas\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
