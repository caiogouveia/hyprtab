#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Modelo da barra de menus (sem dependência do Hyprland nem do D-Bus, testável): árvore de menus,
// geometria da barra e dos menus suspensos, hit-test.
namespace mb {

// ---------- árvore ----------

enum class Toggle { None, Check, Radio };

struct Node {
    int               id = 0;   // id do dbusmenu, ou da ação interna
    std::string       label;    // já sem mnemônico
    std::string       shortcut; // "Ctrl+Shift+S"
    bool              enabled = true, separator = false;
    bool              submenu = false; // tem (ou vai ter, quando o app carregar) filhos
    Toggle            toggle  = Toggle::None;
    bool              checked = false;
    bool              header  = false; // título de seção (negrito, não selecionável)
    bool              slider  = false; // linha com slider (volume): `value` em [0, 1]
    double            value   = 0;
    double            maxLabelW = 0; // num menu: largura máxima do texto dos filhos (0: LABEL_MAX_W)
    std::string       keys;     // menus de atalhos: o que mandar ao app ao clicar ("Ctrl+K Ctrl+O")
    std::vector<Node> children;
};

// "_Arquivo" -> "Arquivo"; "__" vira "_"
std::string stripMnemonic(const std::string& s);

// dbusmenu `shortcut` (aas): {{"Control","Shift","s"}} -> "Ctrl+Shift+S"; vários atalhos separados por ", "
std::string formatShortcut(const std::vector<std::vector<std::string>>& chords);

// nó em `path` (índices nos filhos, a partir de `root`); nullptr se o caminho não existe mais
const Node* at(const Node& root, const std::vector<size_t>& path);

// linha selecionável: não é separador nem cabeçalho e está habilitada
bool selectable(const Node& n);
// próxima linha selecionável a partir de `from` na direção `dir` (+1/-1), dando a volta; -1 se nenhuma
int nextSelectable(const Node& menu, int from, int dir);

// Exec= do .desktop pronto para `sh -c`: tira os códigos de campo (%U, %f, ...) e troca %% por %
std::string stripFieldCodes(const std::string& exec);

// ---------- menus de atalhos ----------
// Para apps que não exportam menu: um arquivo JSON por app descreve os menus com os atalhos do próprio
// app; clicar no item manda o atalho para a janela (dispatcher `sendshortcut` do Hyprland).
//
//   {
//     "name": "Visual Studio Code",
//     "match": ["code", "com.microsoft.vscode"],          // classes da janela (sem diferenciar maiúsculas)
//     "menus": [
//       {"label": "Arquivo", "items": [
//         {"label": "Abrir Pasta…", "keys": "Ctrl+K Ctrl+O"},  // acordes separados por espaço
//         "-",                                                 // separador
//         {"label": "Terminal", "keys": "Ctrl+`", "send": "Ctrl+code:49"},  // mostra `keys`, manda `send`
//         {"label": "Preferências", "items": [ ... ]}          // submenu
//       ]}
//     ]
//   }
//
// Item sem `keys` (nem `send`) aparece desabilitado.

// JSON mínimo (só o necessário para os mapas)
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool                                      boolean = false;
    double                                    number  = 0;
    std::string                               string;
    std::vector<Json>                         array;
    std::vector<std::pair<std::string, Json>> object;

    const Json* get(const std::string& key) const; // nullptr se não é objeto ou não tem a chave
};

// false + `err` ("linha 3: ...") se o texto não é JSON válido
bool parseJson(const std::string& text, Json& out, std::string& err);

// um acorde do `sendshortcut`: modificadores ("CTRL SHIFT") e tecla (nome de keysym ou "code:N")
struct Chord {
    std::string mods, key;
};

// "Ctrl+Shift+P" -> {"CTRL SHIFT", "p"}; "Ctrl+K Ctrl+O" -> dois acordes; "Ctrl+," -> {"CTRL", "comma"}.
// Vazio se não entendeu alguma tecla.
std::vector<Chord> parseKeys(const std::string& keys);

struct AppMap {
    std::string              name, file;
    std::vector<std::string> match; // minúsculas
    Node                     menus; // filhos: os menus da barra
};

bool parseAppMap(const std::string& text, AppMap& out, std::string& err);

// primeiro mapa que casa com alguma das classes (sem diferenciar maiúsculas); nullptr se nenhum
const AppMap* findAppMap(const std::vector<AppMap>& maps, const std::vector<std::string>& classes);

// ---------- geometria ----------
// px lógicos, local ao monitor. `Measure` devolve a largura do texto em px lógicos.

using Measure = std::function<double(const std::string& text, bool bold)>;

struct Box {
    double x = 0, y = 0, w = 0, h = 0;
    bool   contains(double px, double py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// barra
constexpr double BAR_PADX = 8, TITLE_PADX = 9, LOGO_W = 30;

struct Title {
    double x, w;
};

// títulos lado a lado a partir de BAR_PADX: o primeiro é o logo (LOGO_W), os outros têm o texto + padding
std::vector<Title> layoutTitles(const std::vector<double>& textWidths);
// índice do título sob x (só a faixa horizontal; o chamador já sabe que y está na barra); -1 se nenhum
int hitTitle(const std::vector<Title>& titles, double x);

// menu suspenso
constexpr double ROW_H = 22, SEP_H = 10, SLIDER_H = 30, SLIDER_MIN_W = 240, SLIDER_KNOB_R = 7, SLIDER_GAP = 8, MENU_PADY = 5, MENU_PADX = 5, ROW_PADX = 9, CHECK_W = 16, SHORT_GAP = 28, ARROW_W = 16,
                 MENU_MIN_W = 180, MENU_MAX_W = 520, SCREEN_MARGIN = 4,
                 LABEL_MAX_W = 300; // texto maior que isso é cortado com "…" (o menu não fica largo demais)

struct Dropdown {
    Box              box;
    std::vector<Box> rows; // uma por filho (separador incluído)
};

// `s` cortado com "…" para caber em `maxW` (inteiro se já cabe); corta em fronteira de caractere UTF-8
std::string ellipsize(const std::string& s, double maxW, bool bold, const Measure& measure);

// largura do menu para os filhos de `menu`
double dropdownWidth(const Node& menu, const Measure& measure);

// menu de nível 1, embaixo do título `t` (alinhado à esquerda dele), preso dentro do monitor
Dropdown dropdown(const Node& menu, const Measure& measure, const Title& t, double barH, double monW, double monH);
// submenu da linha `row` de `parent`: à direita do menu pai, ou à esquerda se não couber
Dropdown submenu(const Node& menu, const Measure& measure, const Dropdown& parent, size_t row, double monW, double monH);

// índice da linha sob o ponto; -1 dentro do menu fora de linhas; -2 fora do menu
int hitRow(const Dropdown& d, double x, double y);

// trilho do slider dentro da linha e o valor em [0, 1] no ponto x. `pctW`: largura do texto mais largo
// da porcentagem ("100%"), que fica à direita; o trilho para antes dele com folga para o botão inteiro.
Box    sliderTrack(const Box& row, double pctW);
double sliderValueAt(const Box& row, double pctW, double x);

// ---------- área de status (direita da barra) ----------
// Da direita para a esquerda: relógio (clockW, 0 se desligado) e o ícone de volume.
constexpr double CLOCK_PADX = 12, STATUS_GAP = 6, VOLUME_W = 30;

Title volumeTitle(double monW, double clockW);

// ---------- volume (saída do wpctl) ----------

struct Volume {
    double vol   = 0; // 1.0 = 100%
    bool   muted = false;
};
// "Volume: 0.40" / "Volume: 0.40 [MUTED]"; false se não entendeu
bool parseVolume(const std::string& text, Volume& out);

struct Sink {
    int         id = 0;
    std::string name;
    std::string label; // para o menu: sem o nome do dispositivo ("Estéreo digital (HDMI)")
    bool        def = false;
};
// saídas de áudio da seção "Audio > Sinks" do `wpctl status`. O nome de uma saída é "<dispositivo>
// <perfil>"; o `label` fica só com o perfil quando o dispositivo aparece em "Audio > Devices". Se dois
// labels ficariam iguais (dois fones USB "Estéreo analógico"), esses usam shortSinkName(name).
std::vector<Sink> parseSinks(const std::string& status);
// nome mais curto para o menu: tira "(Produto)" do meio do nome ("Ryzen HD Audio Controller (PRIME
// B450M-A Motherboard) Estéreo digital (IEC958)" -> "Ryzen HD Audio Controller Estéreo digital (IEC958)")
std::string shortSinkName(const std::string& name);
// ícone: 0 mudo/zero, 1..3 ondas
int volumeLevel(double vol, bool muted);

// ---------- relógio ----------

// "qua 12 nov  14:32" (sempre em português, sem depender do locale do compositor)
std::string clockText(int wday, int mday, int mon, int hour, int min);

} // namespace mb
