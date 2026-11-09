#pragma once
#include "compat.hpp"

#include <string>
#include <vector>

// Overlay desenhado dentro do compositor (barra central com ícones).
namespace ui {

struct Item {
    std::string addr; // "0x..." (endereço da janela)
    std::string cls;  // classe, para o ícone
    std::string title;
};

void init();     // registra o hook de render
void shutdown();

void show(std::vector<Item> items, size_t selected);
void select(size_t selected);
void hide();
bool visible();

// Hit-test em coordenadas globais do layout.
// >= 0: índice do ícone sob o ponto; -1: dentro da barra mas fora de ícones; -2: fora da barra.
int hit(const Vector2D& global);

} // namespace ui
