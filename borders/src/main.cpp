// hyprborders - bordas e sombra das janelas com o mesmo vidro dos outros plugins (hyprtab, dock, menus),
// tudo dentro do .so (C++).
//
// Duas decorações por janela:
//   sombra    por baixo da janela, como a do macOS: uma camada grande e difusa deslocada para baixo e
//             uma curta rente à borda. A janela em foco tem a sombra bem maior que as outras.
//             Desenhada só em volta da janela (nunca por baixo dela): janelas transparentes não escurecem.
//   contorno  por cima da borda da janela: fio escuro por fora e fio claro por dentro, forte no topo e
//             quase sumindo nas laterais (o reflexo do vidro).
// As duas usam o desenho de sombra e de borda do próprio Hyprland (na GPU): redimensionar não custa nada.
//
// Use com as bordas e a sombra nativas desligadas (general:border_size = 0, decoration:shadow:enabled = false).
//
// Config (hyprlang, até 0.56):
//   plugin {
//       hyprborders {
//           # sombra nos termos do box-shadow do CSS: 0 offset blur spread rgba(0,0,0,alpha)
//           shadow = 1                   # 0 desliga a sombra
//           shadow_blur = 70             # janela em foco: desfoque, px
//           shadow_offset = 22           #   deslocamento para baixo, px
//           shadow_spread = 4            #   crescimento antes do desfoque, px
//           shadow_alpha = 0.56          #   opacidade
//           shadow_blur_inactive = 36    # as outras janelas
//           shadow_offset_inactive = 12
//           shadow_spread_inactive = 0
//           shadow_alpha_inactive = 0.3
//           shadow_contact_alpha = 0.25  # sombra curta rente à borda (0 desliga); nas inativas, 2/3 disso
//           border = 1                  # 0 desliga o contorno
//       }
//   }

#include "../../src/compat.hpp"
#include "model.hpp"

#include <hyprland/src/config/ConfigDataValues.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <vector>

static constexpr auto SCAN_EVERY = std::chrono::milliseconds(250); // janelas novas sem decoração

namespace {

// ---------- config ----------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations" // 0.57 marca a API hyprlang como deprecated

void addConfig() {
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:shadow", Hyprlang::INT{1});
    const borders::Look def;
    const auto i = [](double v) { return Hyprlang::INT{(Hyprlang::INT)v}; };
    const auto f = [](double v) { return Hyprlang::FLOAT{(float)v}; };
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:shadow_blur", i(def.ambientActive.blur));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:shadow_offset", i(def.ambientActive.offsetY));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:shadow_spread", i(def.ambientActive.spread));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:shadow_alpha", f(def.ambientActive.alpha));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:shadow_blur_inactive", i(def.ambientIdle.blur));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:shadow_offset_inactive", i(def.ambientIdle.offsetY));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:shadow_spread_inactive", i(def.ambientIdle.spread));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:shadow_alpha_inactive", f(def.ambientIdle.alpha));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:shadow_contact_alpha", Hyprlang::FLOAT{(float)def.contactActive.alpha});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprborders:border", Hyprlang::INT{1});
}

Hyprlang::INT cfgInt(const char* name, Hyprlang::INT def) {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, name);
    return v ? **(Hyprlang::INT* const*)v->getDataStaticPtr() : def;
}
Hyprlang::FLOAT cfgFloat(const char* name, Hyprlang::FLOAT def) {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, name);
    return v ? **(Hyprlang::FLOAT* const*)v->getDataStaticPtr() : def;
}

#pragma GCC diagnostic pop

struct Config {
    borders::Look look;
    bool          shadow = true, border = true;
} g_cfg;

void loadConfig() {
    g_cfg.shadow            = cfgInt("plugin:hyprborders:shadow", 1);
    g_cfg.border            = cfgInt("plugin:hyprborders:border", 1);
    const borders::Look def;
    auto&               l = g_cfg.look;
    const auto px = [](const char* k, double d, Hyprlang::INT lo, Hyprlang::INT hi) {
        return (double)std::clamp<Hyprlang::INT>(cfgInt(k, (Hyprlang::INT)d), lo, hi);
    };
    l.ambientActive.blur    = px("plugin:hyprborders:shadow_blur", def.ambientActive.blur, 0, 200);
    l.ambientActive.offsetY = px("plugin:hyprborders:shadow_offset", def.ambientActive.offsetY, -100, 100);
    l.ambientActive.spread  = px("plugin:hyprborders:shadow_spread", def.ambientActive.spread, -50, 50);
    l.ambientActive.alpha   = cfgFloat("plugin:hyprborders:shadow_alpha", def.ambientActive.alpha);
    l.ambientIdle.blur      = px("plugin:hyprborders:shadow_blur_inactive", def.ambientIdle.blur, 0, 200);
    l.ambientIdle.offsetY   = px("plugin:hyprborders:shadow_offset_inactive", def.ambientIdle.offsetY, -100, 100);
    l.ambientIdle.spread    = px("plugin:hyprborders:shadow_spread_inactive", def.ambientIdle.spread, -50, 50);
    l.ambientIdle.alpha     = cfgFloat("plugin:hyprborders:shadow_alpha_inactive", def.ambientIdle.alpha);
    l.contactActive.alpha   = cfgFloat("plugin:hyprborders:shadow_contact_alpha", def.contactActive.alpha);
    l.contactIdle.alpha     = l.contactActive.alpha * 2 / 3;
}

// ---------- geometria da janela ----------

// janela com as decorações que fazem parte dela (a barra de título do hyprbars, por exemplo), global,
// na posição animada e com o deslocamento de flutuante.
// Não usa getBoxWithIncludedDecos: ela chama métodos virtuais de todas as decorações da janela (inclusive
// as de um .so recém-descarregado) e derrubou o Hyprland num recarregamento. getWindowDecorationReserved
// só lê o espaço reservado que o posicionador já calculou.
CBox fullWindowBox(const PHLWINDOW& w) {
    const auto pos = w->m_realPosition->value();
    const auto sz  = w->m_realSize->value();
    const auto res = g_pDecorationPositioner->getWindowDecorationReserved(w);
    CBox       b{pos.x - res.topLeft.x, pos.y - res.topLeft.y, sz.x + res.topLeft.x + res.bottomRight.x, sz.y + res.topLeft.y + res.bottomRight.y};
    b.translate(w->m_floatingOffset);
    return b;
}

// a mesma caixa no monitor, em px lógicos, com o deslocamento do workspace trocando
borders::Rect windowRect(const PHLWINDOW& w, const PHLMONITOR& mon) {
    CBox b = fullWindowBox(w);
    if (w->m_workspace && !w->m_pinned)
        b.translate(w->m_workspace->m_renderOffset->value());
    return {b.x - mon->m_position.x, b.y - mon->m_position.y, b.w, b.h};
}

CBox toPx(const borders::Rect& r, double scale) {
    return CBox{r.x * scale, r.y * scale, r.w * scale, r.h * scale}.round();
}

// a janela quer decoração agora? (tela cheia de verdade e a regra `decorate 0` não)
bool wantsDecoration(const PHLWINDOW& w) {
    if (!w || !hyprtabMapped(w))
        return false;
    if (w->isFullscreen() && w->m_workspace && (w->m_workspace->m_fullscreenMode & FSMODE_FULLSCREEN))
        return false;
    if constexpr (requires { w->m_ruleApplicator->decorate().valueOrDefault(); })
        if (!w->m_ruleApplicator->decorate().valueOrDefault())
            return false;
    return true;
}

// ---------- sombra ----------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
// o valor de decoration:shadow:render_power do Hyprland (null se não houver: config em Lua, por exemplo)
Hyprlang::INT* renderPowerPtr() {
    auto* v = HyprlandAPI::getConfigValue(PHANDLE, "decoration:shadow:render_power");
    return v ? *(Hyprlang::INT* const*)v->getDataStaticPtr() : nullptr;
}
#pragma GCC diagnostic pop

class CShadowPass : public IPassElement {
  public:
    struct SData {
        CBox       box;    // caixa da sombra (px do monitor), já com o alcance
        CBox       window; // a janela (px): não recebe sombra
        int        windowRound = 0; // arredondamento da janela (px), para o recorte
        int        round = 0, range = 0; // arredondamento do miolo e largura do esmaecimento (px)
        float      roundingPower = 2.F;
        CHyprColor color;
    };
    explicit CShadowPass(const SData& d) : m_data(d) {}

    void draw(const CRegion&) override {
        // o desenho de sombra do Hyprland só pinta dentro de m_renderData.damage: troca a área, por um
        // instante, pelo que está em volta da janela (e dentro do que precisa ser redesenhado)
        auto&         rd    = HYPRTAB_GL->m_renderData;
        const CRegion saved = rd.damage;
        CRegion       area{m_data.box};
        // tira a janela em forma de cruz: os quadrados dos cantos (do tamanho do arredondamento) ficam com
        // sombra, senão sobra um triângulo sem sombra entre a curva do canto e o retângulo da janela
        const CBox& w = m_data.window;
        const double r = std::clamp((double)m_data.windowRound, 0.0, std::min(w.w, w.h) / 2);
        CRegion cut;
        cut.add(CBox{w.x + r, w.y, w.w - 2 * r, w.h});
        cut.add(CBox{w.x, w.y + r, w.w, w.h - 2 * r});
        area.subtract(cut);
        area.intersect(saved);
        if (area.empty())
            return;
        rd.damage = area;
        HYPRTAB_GL->blend(true);
        // o shader lê decoration:shadow:render_power na hora de desenhar; a nossa curva precisa de
        // SHADOW_POWER (a gaussiana do modelo), então troca só durante este desenho e devolve
        Hyprlang::INT* power = renderPowerPtr();
        const auto     prev  = power ? *power : 0;
        if (power)
            *power = borders::SHADOW_POWER;
        HYPRTAB_GL->renderRoundedShadow(m_data.box, m_data.round, m_data.roundingPower, m_data.range, m_data.color, 1.F);
        if (power)
            *power = prev;
        rd.damage = saved;
    }
    bool needsLiveBlur() override {
        return false;
    }
    bool needsPrecomputeBlur() override {
        return false;
    }
    const char* passName() override {
        return "CHyprbordersShadowPass";
    }

  private:
    SData m_data;
};

class CShadowDeco : public IHyprWindowDecoration {
  public:
    explicit CShadowDeco(PHLWINDOW w) : IHyprWindowDecoration(w), m_window(w) {}

    SDecorationPositioningInfo getPositioningInfo() override {
        const auto e = borders::shadowExtents(g_cfg.look);
        SDecorationPositioningInfo info;
        info.policy         = DECORATION_POSITION_ABSOLUTE;
        info.desiredExtents = {{e.left, e.top}, {e.right, e.bottom}};
        info.edges          = DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT | DECORATION_EDGE_TOP;
        return info;
    }
    void onPositioningReply(const SDecorationPositioningReply&) override {}

    void draw(PHLMONITOR mon, float const& a) override {
        const auto w = m_window.lock();
        if (!g_cfg.shadow || !wantsDecoration(w))
            return;
        const double sc  = mon->m_scale;
        const auto   win = windowRect(w, mon);
        // ambiente primeiro, contato por cima
        for (const auto& s : borders::shadowLayers(g_cfg.look, w == hyprtabFocusedWindow())) {
            const double alpha = s.alpha * a;
            if (alpha <= 0)
                continue;
            const auto         sd = borders::shadowDraw(win, s);
            CShadowPass::SData d;
            d.box           = toPx(sd.box, sc);
            d.window        = toPx(win, sc);
            d.windowRound   = (int)std::lround(w->rounding() * sc);
            d.round         = (int)std::lround(std::max(0.0, w->rounding() - sd.inset) * sc);
            d.roundingPower = w->roundingPower();
            d.range         = (int)std::lround(sd.range * sc);
            d.color         = CHyprColor{0, 0, 0, (float)alpha};
            g_pHyprRenderer->m_renderPass.add(makeUnique<CShadowPass>(d));
        }
    }

    eDecorationType getDecorationType() override {
        return DECORATION_CUSTOM;
    }
    void updateWindow(PHLWINDOW) override {
        damageEntire();
    }
    void damageEntire() override {
        const auto w = m_window.lock();
        if (!w || !hyprtabMapped(w))
            return;
        const auto e = borders::shadowExtents(g_cfg.look);
        const CBox b = fullWindowBox(w);
        g_pHyprRenderer->damageBox(CBox{b.x - e.left, b.y - e.top, b.w + e.left + e.right, b.h + e.top + e.bottom});
    }
    eDecorationLayer getDecorationLayer() override {
        return DECORATION_LAYER_BOTTOM;
    }
    uint64_t getDecorationFlags() override {
        return DECORATION_NON_SOLID;
    }
    std::string getDisplayName() override {
        return "hyprborders: sombra";
    }

  private:
    PHLWINDOWREF m_window;
};

// ---------- contorno ----------

class CBorderDeco : public IHyprWindowDecoration {
  public:
    explicit CBorderDeco(PHLWINDOW w) : IHyprWindowDecoration(w), m_window(w) {}

    SDecorationPositioningInfo getPositioningInfo() override {
        SDecorationPositioningInfo info;
        info.policy         = DECORATION_POSITION_ABSOLUTE;
        info.desiredExtents = {{2, 2}, {2, 2}};
        info.edges          = DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT | DECORATION_EDGE_TOP;
        return info;
    }
    void onPositioningReply(const SDecorationPositioningReply&) override {}

    void draw(PHLMONITOR mon, float const& a) override {
        const auto w = m_window.lock();
        if (!g_cfg.border || !wantsDecoration(w))
            return;
        const double sc    = mon->m_scale;
        const CBox   box   = toPx(windowRect(w, mon), sc);
        const int    lw    = std::max(1, (int)std::lround(sc)); // 1 px lógico
        const double round = w->rounding() * sc;
        if (box.w < 2 * lw + 1 || box.h < 2 * lw + 1)
            return;

        // fio escuro, logo fora da janela
        CBorderPassElement::SBorderData outer;
        outer.box           = box;
        outer.grad1         = CGradientValueData(CHyprColor{0, 0, 0, (float)g_cfg.look.outerA});
        outer.round         = (int)std::lround(round);
        outer.outerRound    = (int)std::lround(round + lw);
        outer.roundingPower = w->roundingPower();
        outer.borderSize    = lw;
        outer.a             = a;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(outer));

        // fio claro, na primeira linha de pixels da janela: forte no topo, fraco nas laterais e embaixo
        CGradientValueData grad;
        grad.m_colors = {CHyprColor{1, 1, 1, (float)g_cfg.look.innerTopA}, CHyprColor{1, 1, 1, (float)g_cfg.look.innerSideA},
                         CHyprColor{1, 1, 1, (float)g_cfg.look.innerSideA}, CHyprColor{1, 1, 1, (float)(g_cfg.look.innerSideA * 0.5)}};
        grad.m_angle  = M_PI / 2; // de cima para baixo
        grad.updateColorsOk();
        CBorderPassElement::SBorderData inner;
        inner.box           = CBox{box}.expand(-lw);
        inner.grad1         = grad;
        inner.round         = (int)std::lround(std::max(0.0, round - lw));
        inner.outerRound    = (int)std::lround(round);
        inner.roundingPower = w->roundingPower();
        inner.borderSize    = lw;
        inner.a             = a;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(inner));
    }

    eDecorationType getDecorationType() override {
        return DECORATION_CUSTOM;
    }
    void updateWindow(PHLWINDOW) override {
        damageEntire();
    }
    void damageEntire() override {
        const auto w = m_window.lock();
        if (!w || !hyprtabMapped(w))
            return;
        const CBox b = fullWindowBox(w);
        g_pHyprRenderer->damageBox(CBox{b.x - 2, b.y - 2, b.w + 4, b.h + 4});
    }
    eDecorationLayer getDecorationLayer() override {
        return DECORATION_LAYER_OVER;
    }
    uint64_t getDecorationFlags() override {
        return DECORATION_NON_SOLID;
    }
    std::string getDisplayName() override {
        return "hyprborders: contorno";
    }

  private:
    PHLWINDOWREF m_window;
};

// ---------- janelas ----------

std::vector<PHLWINDOWREF> g_decorated;
SP<CEventLoopTimer>       g_scan;
SP<HOOK_CALLBACK_FN>      g_openCb;

void decorate(const PHLWINDOW& w) {
    if (!w || std::any_of(g_decorated.begin(), g_decorated.end(), [&](const PHLWINDOWREF& r) { return r.lock() == w; }))
        return;
    HyprlandAPI::addWindowDecoration(PHANDLE, w, makeUnique<CShadowDeco>(w));
    HyprlandAPI::addWindowDecoration(PHANDLE, w, makeUnique<CBorderDeco>(w));
    g_decorated.emplace_back(w);
}

// janelas novas (o evento de abrir janela mudou de forma entre versões: a varredura cobre todas)
void onScan(SP<CEventLoopTimer> self, void*) {
    std::erase_if(g_decorated, [](const PHLWINDOWREF& r) { return r.expired(); });
    loadConfig();
    for (auto& w : hyprtabWindows())
        if (w && hyprtabMapped(w))
            decorate(w);
    self->updateTimeout(SCAN_EVERY);
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;
    addConfig();
    loadConfig();

#ifndef HYPRTAB_EVENT_BUS
    // até a 0.53: a janela já nasce decorada (sem esperar a varredura)
    g_openCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "openWindow", [](void*, SCallbackInfo&, std::any data) {
        try {
            decorate(std::any_cast<PHLWINDOW>(data));
        } catch (...) {}
    });
#endif

    for (auto& w : hyprtabWindows())
        if (w && hyprtabMapped(w))
            decorate(w);

    g_scan = makeShared<CEventLoopTimer>(std::nullopt, onScan, nullptr);
    g_pEventLoopManager->addTimer(g_scan);
    g_scan->updateTimeout(SCAN_EVERY);

    return {"hyprborders", "Bordas e sombra das janelas com o vidro do hyprtab", "caio", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_scan) {
        g_pEventLoopManager->removeTimer(g_scan);
        g_scan.reset();
    }
    g_openCb.reset();
    g_decorated.clear();

    // Tira e destrói as decorações agora, em TODAS as janelas. O Hyprland também as remove ao descarregar,
    // mas só marca (m_decosToRemove) e o updateWindowDecos ignora janela oculta ou desmapeada (outro
    // workspace, janela em fade-out): elas ficariam na janela depois do dlclose, e o próximo
    // damageWindow leria a vtable do .so que não existe mais (SEGV).
    for (auto& w : hyprtabWindows()) {
        if (!w)
            continue;
        std::vector<IHyprWindowDecoration*> mine;
        for (auto& d : w->m_windowDecorations)
            if (dynamic_cast<CShadowDeco*>(d.get()) || dynamic_cast<CBorderDeco*>(d.get()))
                mine.push_back(d.get());
        if (mine.empty())
            continue;
        if (hyprtabMapped(w))
            g_pHyprRenderer->damageWindow(w); // apaga a sombra da tela (com o nosso damageEntire ainda carregado)
        const auto isMine = [&](IHyprWindowDecoration* d) { return std::find(mine.begin(), mine.end(), d) != mine.end(); };
        std::erase_if(w->m_decosToRemove, isMine);
        for (auto* d : mine)
            g_pDecorationPositioner->uncacheDecoration(d);
        std::erase_if(w->m_windowDecorations, [&](const UP<IHyprWindowDecoration>& d) { return isMine(d.get()); });
        g_pDecorationPositioner->forceRecalcFor(w);
    }

    // o render pass guarda os elementos do último quadro até o próximo beginRender; o CShadowPass é
    // deste .so, e destruí-lo depois do dlclose chama código que não existe mais (SEGV em
    // CRenderPass::clear). Some com eles agora, enquanto o destrutor ainda está carregado.
    if (g_pHyprRenderer)
        g_pHyprRenderer->m_renderPass.clear();
}
