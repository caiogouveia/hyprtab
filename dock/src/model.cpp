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
    std::vector<std::string> base;
    for (auto& c : config)
        if (!contains(ov.removed, lower(c)))
            base.push_back(c);
    for (auto& a : ov.added)
        if (!contains(base, lower(a)))
            base.push_back(a);
    if (ov.order.empty())
        return base;

    // primeiro os que têm lugar na ordem salva, nessa ordem; depois os novos, na ordem de antes
    std::vector<std::string> out;
    for (auto& o : ov.order)
        for (auto& b : base)
            if (lower(b) == o && !contains(out, o))
                out.push_back(b);
    for (auto& b : base)
        if (!contains(out, lower(b)))
            out.push_back(b);
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
        else if (line[0] == '=' && !contains(ov.order, key))
            ov.order.push_back(key);
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
    for (auto& o : ov.order)
        out += "=" + o + "\n";
    return out;
}

// ---------- arrastar para reordenar ----------

std::vector<std::string> moveTo(std::vector<std::string> list, const std::string& cls, size_t index) {
    const auto key = lower(cls);
    const auto it  = std::find_if(list.begin(), list.end(), [&](const std::string& x) { return lower(x) == key; });
    if (it == list.end())
        return list;
    const auto value = *it;
    list.erase(it);
    list.insert(list.begin() + (long)std::min(index, list.size()), value);
    return list;
}

void reorderPin(PinOverrides& ov, const std::vector<std::string>& config, const std::string& cls, size_t index) {
    ov.order.clear();
    for (auto& p : moveTo(effectivePins(config, ov), cls, index))
        ov.order.push_back(lower(p));
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

Geom geometry(size_t n, double monW, double monH, double iconSize, double margin, std::vector<size_t> seps) {
    std::erase_if(seps, [&](size_t s) { return s == 0 || s >= n; });
    std::sort(seps.begin(), seps.end());
    seps.erase(std::unique(seps.begin(), seps.end()), seps.end());
    const double cnt  = (double)std::max<size_t>(n, 1);
    const double cell = iconSize + 2 * PAD;
    const double w    = cnt * cell + (cnt - 1) * GAP + 2 * BARPAD + (double)seps.size() * SEP_W;
    const double h    = cell + 2 * BARPAD;
    const double k    = std::clamp((monW - 80) / w, 0.0, 1.0);
    return {(monW - w * k) / 2, monH - margin - h * k, w * k, h * k, iconSize * k, k, seps};
}

namespace {
// quantos separadores vêm antes do item i (inclusive o que fica logo antes dele)
size_t sepsBefore(const Geom& g, size_t i) {
    return (size_t)std::count_if(g.seps.begin(), g.seps.end(), [&](size_t s) { return s <= i; });
}
} // namespace

Cell cell(const Geom& g, size_t i) {
    const double size = g.icon + 2 * PAD * g.k;
    return {g.x + BARPAD * g.k + (double)i * (size + GAP * g.k) + (double)sepsBefore(g, i) * SEP_W * g.k, g.y + BARPAD * g.k, size};
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

double snapToEdge(const Geom& g, double edgeY, double y) {
    const double bottom = g.y + g.h;
    if (y <= bottom || y > edgeY + 1) // +1: o cursor preso na última linha de pixels
        return y;
    return bottom - BARPAD * g.k - 0.5; // dentro da base das células (que crescem para cima)
}

// ---------- arrastar ----------

size_t dropIndex(const Geom& g, size_t n, double x, size_t lo, size_t hi) {
    if (n == 0)
        return 0;
    const double cs  = g.icon + 2 * PAD * g.k;
    const double gap = GAP * g.k;
    const double rel = (x - (g.x + BARPAD * g.k) + gap / 2) / (cs + gap);
    const auto   i   = (size_t)std::clamp(std::floor(rel), 0.0, (double)(n - 1));
    return std::clamp(i, lo, std::max(lo, hi));
}

size_t dragSlot(size_t i, size_t from, size_t drop) {
    const size_t rest = i > from ? i - 1 : i; // posição sem o item arrastado
    return rest >= drop ? rest + 1 : rest;    // abre espaço em `drop`
}

bool dragOutside(const Geom& g, double x, double y) {
    const double cs = g.icon + 2 * PAD * g.k;
    return y < g.y - cs || x < g.x - cs || x > g.x + g.w + cs;
}

// ---------- ampliação ----------

Layout magnify(const Geom& g, size_t n, double mouseX, double mag, double level) {
    Layout       l{g, {}, {}};
    const double cs     = g.icon + 2 * PAD * g.k; // célula sem ampliação
    const double gap    = GAP * g.k;
    const double first  = g.x + BARPAD * g.k;
    const double bottom = g.y + BARPAD * g.k + cs;
    const double amount = std::max(0.0, mag - 1) * std::clamp(level, 0.0, 1.0);
    const double sepw   = SEP_W * g.k;
    // só os separadores que existem para estes n itens
    std::vector<size_t> seps;
    for (auto s : g.seps)
        if (s > 0 && s < n)
            seps.push_back(s);
    auto isSep = [&](size_t i) { return std::find(seps.begin(), seps.end(), i) != seps.end(); };

    // posição original de cada item (com os vãos dos separadores)
    std::vector<double> base(n);
    for (size_t i = 0; i < n; i++)
        base[i] = cell(g, i).x;

    // tamanhos: cos² da distância do cursor ao centro original de cada item
    std::vector<double> size(n);
    for (size_t i = 0; i < n; i++) {
        const double t = std::abs(mouseX - (base[i] + cs / 2)) / (cs * MAG_RANGE);
        const double f = t < 1 ? std::pow(std::cos(t * M_PI / 2), 2) : 0;
        size[i]        = cs * (1 + amount * f);
    }

    // fileira ampliada a partir do mesmo início (os vãos dos separadores não crescem); depois desloca
    // para o ponto sob o cursor ficar parado
    std::vector<double> x(n);
    double              cur = first;
    for (size_t i = 0; i < n; i++) {
        if (isSep(i))
            cur += sepw;
        x[i] = cur;
        cur += size[i] + gap;
    }
    const double baseEnd = first + (double)n * (cs + gap) + (double)seps.size() * sepw, magEnd = cur;

    // onde o ponto mouseX da fileira original foi parar na ampliada. Trechos: cada item com o vão depois
    // dele (só a célula, se vem um separador); e o espaço vazio de cada separador (vão + separador),
    // que não muda de tamanho. Cada trecho é mapeado proporcionalmente para o correspondente.
    double mapped = mouseX;
    if (n > 0 && mouseX > first) {
        bool done = false;
        if (mouseX >= baseEnd) {
            mapped = magEnd + (mouseX - baseEnd);
            done   = true;
        }
        for (auto s : seps)
            if (!done && mouseX >= base[s - 1] + cs && mouseX < base[s]) {
                mapped = x[s - 1] + size[s - 1] + (mouseX - (base[s - 1] + cs));
                done   = true;
            }
        for (size_t j = 0; j < n && !done; j++) {
            const bool   cut = isSep(j + 1);
            const double seg = cut ? cs : cs + gap;
            if (mouseX >= base[j] && mouseX < base[j] + seg) {
                mapped = x[j] + (mouseX - base[j]) / seg * (cut ? size[j] : size[j] + gap);
                done   = true;
            }
        }
    }
    const double shift = mouseX - mapped;

    for (size_t i = 0; i < n; i++)
        l.cells.push_back({x[i] + shift, bottom - size[i], size[i]});
    l.panel.x = g.x + shift;
    l.panel.w = g.w + (magEnd - baseEnd);
    for (auto s : seps)
        l.sepX.push_back(x[s] + shift - (sepw + gap) / 2);
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

std::vector<std::string> newlyOpened(const std::vector<Item>& before, const std::vector<Item>& after) {
    std::vector<std::string> out;
    for (auto& a : after) {
        if (a.windows.empty())
            continue;
        const auto key = lower(a.cls);
        const bool had = std::any_of(before.begin(), before.end(), [&](const Item& b) { return lower(b.cls) == key && !b.windows.empty(); });
        if (!had)
            out.push_back(a.cls);
    }
    return out;
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

// ---------- entrada ----------

double enterOffset(double ms, double distance) {
    if (ms <= 0)
        return distance;
    if (ms >= ENTER_MS)
        return 0;
    // easeOutBack: chega a 1 passando um pouco (o deslocamento fica levemente negativo) e volta
    const double t = ms / ENTER_MS - 1, c = ENTER_OVERSHOOT;
    const double e = 1 + (c + 1) * t * t * t + c * t * t;
    return distance * (1 - e);
}

// ---------- puff ----------

namespace {
double easeOut(double t) {
    return 1 - std::pow(1 - std::clamp(t, 0.0, 1.0), 3);
}

// pseudoaleatório fixo em [0, 1) para a partícula `i`, canal `c`
double rnd(int i, int c) {
    const double v = std::sin(i * 12.9898 + c * 78.233 + 0.5) * 43758.5453;
    return v - std::floor(v);
}
} // namespace

std::vector<Puff> poofPuffs(double t, double icon) {
    std::vector<Puff> out;
    if (t < 0 || t >= 1)
        return out;

    // clarão: núcleo claro que cresce e some no primeiro quinto
    if (t < 0.2) {
        const double f = t / 0.2;
        out.push_back({0, 0, icon * (0.6 + 0.9 * easeOut(f)), 0.75 * (1 - f) * (1 - f), -1});
    }

    for (int i = 0; i < POOF_PARTICLES; i++) {
        const double life = 0.65 + 0.35 * rnd(i, 0); // cada uma some no seu tempo
        const double u    = t / life;                 // tempo próprio 0..1
        if (u >= 1)
            continue;
        const double ang   = 2 * M_PI * (i + 0.6 * rnd(i, 1)) / POOF_PARTICLES;
        const double dist  = icon * (0.35 + 0.75 * rnd(i, 2)) * easeOut(u); // sai rápido, desacelera
        const double rise  = icon * (0.25 + 0.35 * rnd(i, 3)) * u * u;     // fumaça quente sobe, cada vez mais
        const double size0 = icon * (0.35 + 0.30 * rnd(i, 4));
        const double grow  = 1.6 + 1.2 * rnd(i, 5);
        const double peak  = 0.35 + 0.35 * rnd(i, 6);
        const double in    = std::min(1.0, u / 0.08);         // aparece rápido
        const double outA  = std::pow(1 - u, 1.6);            // some devagar
        out.push_back({std::cos(ang) * dist, std::sin(ang) * dist * 0.8 - rise, size0 * (1 + (grow - 1) * easeOut(u)), peak * in * outA,
                       (int)(rnd(i, 7) * POOF_VARIANTS) % POOF_VARIANTS});
    }
    return out;
}

double poofIconScale(double t) {
    return t >= POOF_ICON_END ? 0 : 1 - 0.7 * easeOut(t / POOF_ICON_END);
}

double poofIconAlpha(double t) {
    return t >= POOF_ICON_END ? 0 : 1 - t / POOF_ICON_END;
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

namespace {
double menuHeight(const std::vector<bool>& seps) {
    double h = 2 * MENU_PAD;
    for (bool s : seps)
        h += s ? MENU_SEP : MENU_ROW;
    return h;
}
} // namespace

MenuGeom menuGeometry(const Frame& f, const Box& cell, const Box& panel, const std::vector<bool>& seps) {
    const auto b = besideItem(f, cell, panel, MENU_W, menuHeight(seps), MENU_GAP);
    return {b.x, b.y, b.w, b.h};
}

double menuRowY(const MenuGeom& m, const std::vector<bool>& seps, size_t row) {
    double y = m.y + MENU_PAD;
    for (size_t i = 0; i < row && i < seps.size(); i++)
        y += seps[i] ? MENU_SEP : MENU_ROW;
    return y;
}

int menuHit(const MenuGeom& m, const std::vector<bool>& seps, double lx, double ly) {
    if (lx < m.x || lx > m.x + m.w || ly < m.y || ly > m.y + m.h)
        return -2;
    for (size_t i = 0; i < seps.size(); i++) {
        const double y = menuRowY(m, seps, i), h = seps[i] ? MENU_SEP : MENU_ROW;
        if (ly >= y && ly < y + h)
            return seps[i] ? -1 : (int)i;
    }
    return -1;
}

MenuGeom submenuGeometry(const MenuGeom& parent, double rowY, size_t rows, double monW, double monH) {
    const double w = MENU_W, h = (double)rows * MENU_ROW + 2 * MENU_PAD;
    double       x = parent.x + parent.w - 2;
    if (x + w > monW - 4)
        x = parent.x - w + 2;
    const double y = std::clamp(rowY - MENU_PAD, 4.0, std::max(4.0, monH - h - 4));
    return {std::max(4.0, x), y, w, h};
}

double sliderValue(double x, double x0, double x1, double lo, double hi) {
    if (x1 <= x0)
        return lo;
    return lo + std::clamp((x - x0) / (x1 - x0), 0.0, 1.0) * (hi - lo);
}

double sliderX(double v, double x0, double x1, double lo, double hi) {
    if (hi <= lo)
        return x0;
    return x0 + std::clamp((v - lo) / (hi - lo), 0.0, 1.0) * (x1 - x0);
}

} // namespace dock
