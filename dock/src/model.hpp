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

// listas da config e janelas ignoradas: splitList / classIgnored em ../../src/clients.hpp

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
    std::vector<std::string> order;          // ordem dos fixos definida arrastando ("=classe"); vazio: a da config
};

std::vector<std::string> effectivePins(const std::vector<std::string>& config, const PinOverrides& ov);
void                     setPinned(PinOverrides& ov, const std::vector<std::string>& config, const std::string& cls, bool pin);
PinOverrides             parsePins(const std::string& text);
std::string              serializePins(const PinOverrides& ov);

// ---------- arrastar para reordenar ----------

// `list` com `cls` movido para a posição `index` (limitada ao tamanho); sem diferenciar maiúsculas
std::vector<std::string> moveTo(std::vector<std::string> list, const std::string& cls, size_t index);
// grava a ordem dos fixos depois de arrastar `cls` para `index`
void reorderPin(PinOverrides& ov, const std::vector<std::string>& config, const std::string& cls, size_t index);

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

// arrastando: posição (0..n-1) onde o item cairia com o cursor em `x`, presa em [lo, hi]
size_t dropIndex(const Geom& g, size_t n, double x, size_t lo, size_t hi);
// arrastando o item `from` para a posição `drop`: em que posição (célula) fica o item `i` (i != from)
size_t dragSlot(size_t i, size_t from, size_t drop);
// arrastando: o cursor (espaço da dock) saiu da dock o bastante para soltar = remover dos fixos?
// Mais de uma célula para dentro da tela, ou além das pontas da fileira.
bool dragOutside(const Geom& g, double x, double y);

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
constexpr int    BOUNCE_APPEAR = 2; // pulos de um app aberto por fora da dock, ao aparecer a janela

// quanto o ícone sobe (px) `ms` depois do início do pulo, para uma célula de `cell` px
double bounceOffset(double ms, double cell);

// apps que acabaram de abrir: têm janela em `after` e não tinham (ou nem estavam) em `before`
std::vector<std::string> newlyOpened(const std::vector<Item>& before, const std::vector<Item>& after);
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

// ---------- puff ao remover ----------
// Soltar um fixo longe da dock: o ícone encolhe e some enquanto uma nuvem de fumaça se espalha do
// ponto. São partículas macias (texturas de nuvem, ver render) com direção, velocidade, tamanho,
// tom e duração próprios (pseudoaleatórios fixos), que desaceleram, sobem como fumaça e desvanecem
// cada uma no seu tempo; um clarão no centro marca o início. Tudo relativo ao centro e ao ícone.

constexpr double POOF_MS = 700, POOF_ICON_END = 0.3; // duração; fração em que o ícone já sumiu
constexpr int    POOF_PARTICLES = 22, POOF_VARIANTS = 6;  // variantes = 3 formatos x 2 tons

struct Puff {
    double dx, dy;  // centro da partícula em relação ao ponto (px; y para baixo)
    double size;    // lado (px)
    double alpha;
    int    variant; // qual textura de nuvem (0..POOF_VARIANTS-1); -1 = clarão
};

std::vector<Puff> poofPuffs(double t, double icon); // vazio quando t >= 1
double            poofIconScale(double t);           // 1 -> 0.3 até POOF_ICON_END, depois 0
double            poofIconAlpha(double t);           // idem, de 1 a 0

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
