#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Modelo da dock (sem dependência do Hyprland, testável): itens, geometria e hit-test.
namespace dock {

// ---------- itens ----------

struct Item {
    std::string              cls;     // classe da janela (ou id do .desktop, para os fixos)
    bool                     pinned = false;
    std::vector<std::string> windows; // endereços "0x..." das janelas abertas desse app
};

// "a, b ,,c" -> {"a", "b", "c"}
std::vector<std::string> parseList(const std::string& csv);

// Fixos primeiro (na ordem da config), depois os apps abertos que não são fixos. A classe casa
// sem diferenciar maiúsculas. `order` guarda a ordem em que os não fixos apareceram e é
// atualizada aqui: apps novos entram no fim, os que fecharam saem; assim a dock não embaralha.
// `windows`: pares {endereço, classe}.
std::vector<Item> buildItems(const std::vector<std::string>& pinned, const std::vector<std::pair<std::string, std::string>>& windows,
                             std::vector<std::string>& order, bool showRunning = true);

// Janela a focar ao clicar no item: se a focada já é dele, a próxima (dá a volta); senão a primeira.
// Vazio se o app não tem janela aberta.
std::string nextWindow(const Item& item, const std::string& focusedAddr);

// Janela alvo de maximizar/minimizar: a focada, se for do app; senão a primeira. Vazio se não há.
std::string targetWindow(const Item& item, const std::string& focusedAddr);

// ---------- fixos ----------
// O `apps` da config é a base; o que o usuário fixa/desafixa pelo menu fica num arquivo de estado
// como ajuste por cima dela ("+classe" / "-classe" por linha), para sobreviver a reloads.

struct PinOverrides {
    std::vector<std::string> added, removed; // minúsculas
};

std::vector<std::string> effectivePins(const std::vector<std::string>& config, const PinOverrides& ov);
void                     setPinned(PinOverrides& ov, const std::vector<std::string>& config, const std::string& cls, bool pin);
PinOverrides             parsePins(const std::string& text);
std::string              serializePins(const PinOverrides& ov);

// Exec= do .desktop pronto para `sh -c`: tira os códigos de campo (%U, %f, ...) e troca %% por %.
std::string stripFieldCodes(const std::string& exec);

// ---------- geometria ----------
// px lógicos, local ao monitor. A dock fica centralizada embaixo, a `margin` px da borda.

constexpr double PAD = 6, GAP = 4, BARPAD = 8;

struct Geom {
    double x, y, w, h; // painel
    double icon;       // lado do ícone (já reduzido por k)
    double k;          // fator de redução quando os ícones não cabem na largura
};

struct Cell {
    double x, y, size; // quadrado do item i (fundo do hover)
};

Geom geometry(size_t n, double monW, double monH, double iconSize, double margin);
Cell cell(const Geom& g, size_t i);

// >= 0: índice do item sob o ponto; -1: dentro do painel mas fora de itens; -2: fora do painel.
int hit(const Geom& g, size_t n, double lx, double ly);

// ---------- ampliação (zoom estilo macOS) ----------
// Os itens perto do cursor crescem (queda suave até MAG_RANGE células de distância), para cima,
// com a base alinhada à da dock. O ponto da fileira sob o cursor fica parado e o resto se afasta,
// então o item sob o mouse não foge dele. `level` (0..1) é a animação de entrada/saída.

constexpr double MAG_RANGE = 2.5;

struct Layout {
    Geom              panel; // painel ampliado (mesma altura; só a largura e o x mudam)
    std::vector<Cell> cells;
};

Layout magnify(const Geom& g, size_t n, double mouseX, double mag, double level);
// igual a hit(), mas nos itens já ampliados (que podem passar do topo do painel)
int hit(const Layout& l, double lx, double ly);

// ---------- pulo ao abrir (estilo macOS) ----------
// O ícone pula enquanto o app abre: parábola (gravidade), altura em fração da célula. Ao parar,
// termina o pulo em andamento e pousa em vez de sumir no ar.

constexpr double BOUNCE_PERIOD = 600, BOUNCE_HEIGHT = 0.5, BOUNCE_TIMEOUT = 8000; // ms, fração, ms

// quanto o ícone sobe (px) `ms` depois do início do pulo, para uma célula de `cell` px
double bounceOffset(double ms, double cell);
// momento (ms desde o início) em que o ícone pousa, se mandarem parar em `stopMs`
double bounceLanding(double stopMs);

// ---------- borda da tela ----------
// Todo o layout acima é feito como se a dock estivesse embaixo ("espaço da dock": fileira no x,
// borda da tela em y = altura). Para esquerda/direita o espaço da dock tem as dimensões do monitor
// trocadas e as caixas são convertidas na hora de desenhar; o mouse faz o caminho inverso.
// Assim zoom, pulo e bolinha já apontam para o centro / para a borda certa.

enum class Edge { Bottom, Left, Right };

Edge parseEdge(const std::string& s); // "left" / "right"; qualquer outra coisa é bottom

struct Box {
    double x, y, w, h;
};

struct Frame {
    Edge   edge;
    double monW, monH;   // monitor (px lógicos)
    double w() const;    // largura do espaço da dock (ao longo da fileira)
    double h() const;    // altura do espaço da dock (até a borda)
};

Box  toScreen(const Frame& f, const Box& b);                       // espaço da dock -> monitor
void toDock(const Frame& f, double sx, double sy, double& dx, double& dy); // monitor -> espaço da dock

// ---------- menu de contexto e tooltip ----------
// Texto não gira: menu e tooltip são posicionados direto no monitor, do lado de dentro da dock
// (acima dela embaixo, à direita na esquerda, à esquerda na direita), centrados no item e presos
// dentro do monitor. `cell` e `panel` já em coordenadas do monitor.

constexpr double MENU_W = 200, MENU_ROW = 30, MENU_PAD = 6, MENU_GAP = 8;

struct MenuGeom {
    double x, y, w, h;
};

MenuGeom menuGeometry(const Frame& f, const Box& cell, const Box& panel, size_t rows);
Box      besideItem(const Frame& f, const Box& cell, const Box& panel, double w, double h, double gap);
// índice da linha sob o ponto; -1 dentro do menu fora de linhas; -2 fora do menu
int menuHit(const MenuGeom& m, size_t rows, double lx, double ly);

} // namespace dock
