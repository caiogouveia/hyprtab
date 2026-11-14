#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Modelo do pager (sem dependência do Hyprland, testável): onde ficam as telas e as placas, como o
// monitor cabe dentro de cada tela, hit-test.
//
// Coordenadas em px lógicos. O layout é local ao pager (origem no canto de cima à esquerda dele);
// `place` diz onde o pager fica no monitor.
namespace pager {

struct Box {
    double x = 0, y = 0, w = 0, h = 0;
    bool   contains(double px, double py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// proporções do desenho, em fração da largura da tela (`screenW`)
constexpr double SCREEN_ASPECT = 1.36; // tela de TV (largura / altura), não a do monitor
constexpr double GAP           = 0.40; // entre uma tela e a de baixo (espaço do cano que sobe)
constexpr double TUBE          = 0.075; // espessura dos canos
constexpr double BRACKET       = 0.34; // lado da placa
constexpr double BRACKET_DX    = 0.30; // pivô do refletor, à direita da tela
constexpr double RADIUS        = 0.19; // raio do canto da tela (fração da largura)

struct Layout {
    double           screenW = 0, screenH = 0, tube = 0;
    std::vector<Box> screens;  // vidro de cada tela
    std::vector<Box> brackets; // placa de cada tela (quadrado em volta do centro)
    double           w = 0, h = 0; // tamanho do pager
};

Layout layout(size_t n, double screenW);

enum class Edge { Left, Right };
Edge parseEdge(const std::string& s); // "right"; qualquer outra coisa é left

enum class Align { Top, Center, Bottom };
Align parseAlign(const std::string& s); // "top", "bottom"; qualquer outra coisa é center

// área reservada nas bordas do monitor (dock, barras): o pager fica do lado de dentro dela
struct Reserved {
    double top = 0, right = 0, bottom = 0, left = 0;
};

// onde o pager fica no monitor
struct Placement {
    Edge   edge   = Edge::Left;
    Align  align  = Align::Center;
    double margin = 12; // da borda (e, alinhado em cima/embaixo, também do topo/da base), px
    double offset = 0;  // deslocamento vertical depois de alinhar, px (positivo desce)
};

// pager no monitor: na borda escolhida, a `margin` px da área reservada daquela borda; na vertical,
// alinhado no espaço livre (em cima/embaixo, a `margin` px dele), mais `offset`, preso na tela
Box place(const Layout& l, const Placement& p, double monW, double monH, const Reserved& r = {});

// o pager aparece no monitor `name`? `filter` vazio: em todos; senão, nomes separados por vírgula
bool monitorMatches(const std::string& filter, const std::string& name);

// tela sob o ponto (local ao pager): índice, ou -1. A placa conta como parte da tela.
int hitScreen(const Layout& l, double x, double y);

// o monitor (monW x monH) encaixado no vidro, mantendo a proporção (sobra faixa preta em cima e embaixo
// ou dos lados), com uma pequena margem para não encostar na moldura
Box fitMonitor(const Box& glass, double monW, double monH);

// janela (x, y, w, h no monitor, relativas ao canto dele) -> retângulo dentro de `area` (fitMonitor),
// cortado nas bordas da área. Largura/altura 0 se ficou toda fora.
Box thumb(const Box& area, double monW, double monH, const Box& win);

// rótulo do workspace: o nome, se não for só o número; vazio se for (a posição já diz o número)
std::string workspaceLabel(int id, const std::string& name);

} // namespace pager
