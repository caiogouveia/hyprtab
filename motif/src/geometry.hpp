#pragma once
// Geometria da moldura Motif, em px lógicos, relativa ao canto da moldura. Sem dependência do Hyprland.
//
//   +--------------------------------------+  <- borda (border px) em volta de tudo
//   | [M] ----- título centralizado ---[_][]|  <- barra (title px de altura)
//   +--------------------------------------+
//   |            área do cliente           |
//   +--------------------------------------+

namespace motif {

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool   contains(double px, double py) const {
        return w > 0 && h > 0 && px >= x && px < x + w && py >= y && py < y + h;
    }
    Rect inset(double d) const {
        return {x + d, y + d, w > 2 * d ? w - 2 * d : 0, h > 2 * d ? h - 2 * d : 0};
    }
};

enum class Part { None, Client, Frame, Title, Menu, Minimize, Maximize };

struct Layout {
    Rect bar;      // barra de título inteira
    Rect client;   // área do cliente (o "buraco" da moldura)
    Rect menu;     // botão de menu (esquerda); w == 0 se não cabe
    Rect minimize; // botão de minimizar (direita); w == 0 se não cabe
    Rect maximize; // botão de maximizar (extrema direita); w == 0 se não cabe
    Rect text;     // faixa do título, entre os botões
};

// w×h: tamanho total da moldura (cliente + extensões)
Layout layout(double w, double h, int border, int title);

// parte sob o ponto (x, y); None fora da moldura
Part hit(const Layout& l, double w, double h, double x, double y);

} // namespace motif
