#include "model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace dock {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return "";
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

} // namespace

// ---------- itens ----------

std::vector<std::string> parseList(const std::string& csv) {
    std::vector<std::string> out;
    size_t                   start = 0;
    while (start <= csv.size()) {
        const auto end = std::min(csv.find(',', start), csv.size());
        if (auto v = trim(csv.substr(start, end - start)); !v.empty())
            out.push_back(std::move(v));
        start = end + 1;
    }
    return out;
}

std::vector<Item> buildItems(const std::vector<std::string>& pinned, const std::vector<std::pair<std::string, std::string>>& windows,
                             std::vector<std::string>& order, bool showRunning) {
    std::vector<Item> items;
    std::set<std::string> pinnedKeys;
    for (auto& p : pinned)
        if (pinnedKeys.insert(lower(p)).second) // fixo repetido na config aparece uma vez só
            items.push_back({p, true, {}});

    // classe (minúscula) -> índice em items, para os fixos
    auto find = [&](const std::string& key) -> Item* {
        for (auto& it : items)
            if (lower(it.cls) == key)
                return &it;
        return nullptr;
    };

    // agrupa as janelas abertas por classe
    std::vector<Item> running;
    for (auto& [addr, cls] : windows) {
        if (cls.empty())
            continue;
        const auto key = lower(cls);
        if (auto* p = find(key)) {
            p->windows.push_back(addr);
            continue;
        }
        auto r = std::find_if(running.begin(), running.end(), [&](const Item& i) { return lower(i.cls) == key; });
        if (r == running.end())
            running.push_back({cls, false, {addr}});
        else
            r->windows.push_back(addr);
    }

    // ordem estável dos não fixos: remove quem fechou, acrescenta os novos no fim
    std::erase_if(order, [&](const std::string& key) {
        return std::none_of(running.begin(), running.end(), [&](const Item& i) { return lower(i.cls) == key; });
    });
    for (auto& r : running)
        if (std::find(order.begin(), order.end(), lower(r.cls)) == order.end())
            order.push_back(lower(r.cls));

    if (showRunning)
        for (auto& key : order)
            for (auto& r : running)
                if (lower(r.cls) == key)
                    items.push_back(r);

    // janelas em ordem fixa (por endereço) para o clique ciclar sempre na mesma sequência
    for (auto& it : items)
        std::sort(it.windows.begin(), it.windows.end());
    return items;
}

std::string nextWindow(const Item& item, const std::string& focusedAddr) {
    if (item.windows.empty())
        return "";
    const auto it = std::find(item.windows.begin(), item.windows.end(), focusedAddr);
    if (it == item.windows.end())
        return item.windows.front();
    return std::next(it) == item.windows.end() ? item.windows.front() : *std::next(it);
}

std::string targetWindow(const Item& item, const std::string& focusedAddr) {
    if (item.windows.empty())
        return "";
    const bool focusedIsOurs = std::find(item.windows.begin(), item.windows.end(), focusedAddr) != item.windows.end();
    return focusedIsOurs ? focusedAddr : item.windows.front();
}

// ---------- fixos ----------

namespace {
bool contains(const std::vector<std::string>& v, const std::string& key) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& x) { return lower(x) == key; });
}
void erase(std::vector<std::string>& v, const std::string& key) {
    std::erase_if(v, [&](const std::string& x) { return lower(x) == key; });
}
} // namespace

std::vector<std::string> effectivePins(const std::vector<std::string>& config, const PinOverrides& ov) {
    std::vector<std::string> out;
    for (auto& c : config)
        if (!contains(ov.removed, lower(c)))
            out.push_back(c);
    for (auto& a : ov.added)
        if (!contains(out, lower(a)))
            out.push_back(a);
    return out;
}

void setPinned(PinOverrides& ov, const std::vector<std::string>& config, const std::string& cls, bool pin) {
    const auto key      = lower(cls);
    const bool inConfig = contains(config, key);
    erase(ov.added, key);
    erase(ov.removed, key);
    if (pin && !inConfig)
        ov.added.push_back(key);
    else if (!pin && inConfig)
        ov.removed.push_back(key);
}

PinOverrides parsePins(const std::string& text) {
    PinOverrides ov;
    size_t       start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        const auto line = trim(text.substr(start, end - start));
        start           = end + 1;
        if (line.size() < 2)
            continue;
        const auto key = lower(trim(line.substr(1)));
        if (key.empty())
            continue;
        if (line[0] == '+' && !contains(ov.added, key))
            ov.added.push_back(key);
        else if (line[0] == '-' && !contains(ov.removed, key))
            ov.removed.push_back(key);
    }
    return ov;
}

std::string serializePins(const PinOverrides& ov) {
    std::string out;
    for (auto& a : ov.added)
        out += "+" + a + "\n";
    for (auto& r : ov.removed)
        out += "-" + r + "\n";
    return out;
}

std::string stripFieldCodes(const std::string& exec) {
    std::string out;
    for (size_t i = 0; i < exec.size(); i++) {
        if (exec[i] != '%') {
            out += exec[i];
            continue;
        }
        if (i + 1 < exec.size() && exec[i + 1] == '%')
            out += '%';
        i++; // pula a letra do código (ou o segundo %)
    }
    return trim(out);
}

// ---------- geometria ----------

Geom geometry(size_t n, double monW, double monH, double iconSize, double margin) {
    const double cnt  = (double)std::max<size_t>(n, 1);
    const double cell = iconSize + 2 * PAD;
    const double w    = cnt * cell + (cnt - 1) * GAP + 2 * BARPAD;
    const double h    = cell + 2 * BARPAD;
    const double k    = std::clamp((monW - 80) / w, 0.0, 1.0);
    return {(monW - w * k) / 2, monH - margin - h * k, w * k, h * k, iconSize * k, k};
}

Cell cell(const Geom& g, size_t i) {
    const double size = g.icon + 2 * PAD * g.k;
    return {g.x + BARPAD * g.k + (double)i * (size + GAP * g.k), g.y + BARPAD * g.k, size};
}

int hit(const Geom& g, size_t n, double lx, double ly) {
    if (lx < g.x || lx > g.x + g.w || ly < g.y || ly > g.y + g.h)
        return -2;
    for (size_t i = 0; i < n; i++) {
        const auto c = cell(g, i);
        if (lx >= c.x && lx <= c.x + c.size && ly >= c.y && ly <= c.y + c.size)
            return (int)i;
    }
    return -1;
}

// ---------- ampliação ----------

Layout magnify(const Geom& g, size_t n, double mouseX, double mag, double level) {
    Layout       l{g, {}};
    const double cs     = g.icon + 2 * PAD * g.k; // célula sem ampliação
    const double gap    = GAP * g.k;
    const double first  = g.x + BARPAD * g.k;
    const double bottom = g.y + BARPAD * g.k + cs;
    const double amount = std::max(0.0, mag - 1) * std::clamp(level, 0.0, 1.0);

    // tamanhos: cos² da distância do cursor ao centro original de cada item
    std::vector<double> size(n);
    for (size_t i = 0; i < n; i++) {
        const double t = std::abs(mouseX - (first + (double)i * (cs + gap) + cs / 2)) / (cs * MAG_RANGE);
        const double f = t < 1 ? std::pow(std::cos(t * M_PI / 2), 2) : 0;
        size[i]        = cs * (1 + amount * f);
    }

    // fileira ampliada a partir do mesmo início; depois desloca para o ponto sob o cursor ficar parado
    std::vector<double> x(n);
    double              cur = first;
    for (size_t i = 0; i < n; i++) {
        x[i] = cur;
        cur += size[i] + gap;
    }
    const double baseEnd = first + (double)n * (cs + gap), magEnd = cur;

    double mapped; // onde o ponto mouseX da fileira original foi parar na ampliada
    if (n == 0 || mouseX <= first)
        mapped = mouseX;
    else if (mouseX >= baseEnd)
        mapped = magEnd + (mouseX - baseEnd);
    else {
        const size_t j    = std::min(n - 1, (size_t)((mouseX - first) / (cs + gap)));
        const double frac = (mouseX - (first + (double)j * (cs + gap))) / (cs + gap);
        mapped            = x[j] + frac * (size[j] + gap);
    }
    const double shift = mouseX - mapped;

    for (size_t i = 0; i < n; i++)
        l.cells.push_back({x[i] + shift, bottom - size[i], size[i]});
    l.panel.x = g.x + shift;
    l.panel.w = g.w + (magEnd - baseEnd);
    return l;
}

int hit(const Layout& l, double lx, double ly) {
    for (size_t i = 0; i < l.cells.size(); i++) {
        const auto& c = l.cells[i];
        if (lx >= c.x && lx <= c.x + c.size && ly >= c.y && ly <= c.y + c.size)
            return (int)i;
    }
    const auto& p = l.panel;
    return lx >= p.x && lx <= p.x + p.w && ly >= p.y && ly <= p.y + p.h ? -1 : -2;
}

// ---------- pulo ----------

double bounceOffset(double ms, double cell) {
    if (ms <= 0)
        return 0;
    const double p = std::fmod(ms, BOUNCE_PERIOD) / BOUNCE_PERIOD; // 0..1 dentro de um pulo
    return cell * BOUNCE_HEIGHT * 4 * p * (1 - p);
}

double bounceLanding(double stopMs) {
    return std::ceil(std::max(0.0, stopMs) / BOUNCE_PERIOD) * BOUNCE_PERIOD;
}

// ---------- borda da tela ----------

Edge parseEdge(const std::string& s) {
    const auto v = lower(trim(s));
    return v == "left" ? Edge::Left : v == "right" ? Edge::Right : Edge::Bottom;
}

double Frame::w() const {
    return edge == Edge::Bottom ? monW : monH;
}
double Frame::h() const {
    return edge == Edge::Bottom ? monH : monW;
}

// embaixo: identidade. Direita: transposta (fileira desce no y, borda em x = monW).
// Esquerda: transposta e espelhada (borda em x = 0).
Box toScreen(const Frame& f, const Box& b) {
    switch (f.edge) {
        case Edge::Bottom: return b;
        case Edge::Right: return {b.y, b.x, b.h, b.w};
        case Edge::Left: return {f.monW - b.y - b.h, b.x, b.h, b.w};
    }
    return b;
}

void toDock(const Frame& f, double sx, double sy, double& dx, double& dy) {
    switch (f.edge) {
        case Edge::Bottom: dx = sx, dy = sy; return;
        case Edge::Right: dx = sy, dy = sx; return;
        case Edge::Left: dx = sy, dy = f.monW - sx; return;
    }
}

// ---------- menu de contexto e tooltip ----------

Box besideItem(const Frame& f, const Box& cell, const Box& panel, double w, double h, double gap) {
    const double cx = cell.x + cell.w / 2, cy = cell.y + cell.h / 2;
    double       x = 0, y = 0;
    switch (f.edge) {
        case Edge::Bottom: x = cx - w / 2, y = std::min(panel.y, cell.y) - gap - h; break;
        case Edge::Left: x = std::max(panel.x + panel.w, cell.x + cell.w) + gap, y = cy - h / 2; break;
        case Edge::Right: x = std::min(panel.x, cell.x) - gap - w, y = cy - h / 2; break;
    }
    // preso no monitor ao longo da dock (o outro eixo já fica do lado de dentro)
    x = std::clamp(x, 4.0, std::max(4.0, f.monW - w - 4));
    y = std::clamp(y, 4.0, std::max(4.0, f.monH - h - 4));
    return {x, y, w, h};
}

MenuGeom menuGeometry(const Frame& f, const Box& cell, const Box& panel, size_t rows) {
    const auto b = besideItem(f, cell, panel, MENU_W, (double)rows * MENU_ROW + 2 * MENU_PAD, MENU_GAP);
    return {b.x, b.y, b.w, b.h};
}

int menuHit(const MenuGeom& m, size_t rows, double lx, double ly) {
    if (lx < m.x || lx > m.x + m.w || ly < m.y || ly > m.y + m.h)
        return -2;
    const double rel = ly - m.y - MENU_PAD;
    if (rel < 0 || rel >= (double)rows * MENU_ROW)
        return -1;
    return (int)(rel / MENU_ROW);
}

} // namespace dock
