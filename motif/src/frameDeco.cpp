#include "frameDeco.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <pango/pangocairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

#include "FramePassElement.hpp"

using motif::Part;
using motif::Rect;

namespace {

constexpr uint32_t BTN_LEFT_ = 0x110;

// bisel: clareia/escurece a cor da face (o Motif real faz o mesmo a partir da cor de fundo)
CHyprColor lighter(CHyprColor c) {
    return CHyprColor{c.r + (1.F - c.r) * 0.55F, c.g + (1.F - c.g) * 0.55F, c.b + (1.F - c.b) * 0.55F, c.a};
}
CHyprColor darker(CHyprColor c) {
    return CHyprColor{c.r * 0.45F, c.g * 0.45F, c.b * 0.45F, c.a};
}

// "dispatcher argumento" -> chama o dispatcher do Hyprland (age na janela ativa)
void runDispatch(const std::string& spec) {
    if (spec.empty())
        return;
    const auto  sp   = spec.find(' ');
    const auto  name = spec.substr(0, sp);
    const auto  arg  = sp == std::string::npos ? std::string{} : spec.substr(spec.find_first_not_of(' ', sp));
    const auto  it   = g_pKeybindManager->m_dispatchers.find(name);
    if (it != g_pKeybindManager->m_dispatchers.end())
        it->second(arg);
}

} // namespace

CMotifFrame::CMotifFrame(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
    m_window = pWindow;

    if (const auto PMONITOR = pWindow->m_monitor.lock())
        PMONITOR->m_scheduledRecalc = true;

    m_mouseButtonCb = HyprlandAPI::registerCallbackDynamic(
        PHANDLE, "mouseButton", [&](void* self, SCallbackInfo& info, std::any param) { onMouseButton(info, std::any_cast<IPointer::SButtonEvent>(param)); });
    m_mouseMoveCb = HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseMove", [&](void* self, SCallbackInfo& info, std::any param) { onMouseMove(); });

    m_textTex = makeShared<CTexture>();
}

CMotifFrame::~CMotifFrame() {
    HyprlandAPI::unregisterCallback(PHANDLE, m_mouseButtonCb);
    HyprlandAPI::unregisterCallback(PHANDLE, m_mouseMoveCb);
    std::erase(g_pGlobalState->frames, m_self);
}

// ---------- posicionamento ----------

SDecorationPositioningInfo CMotifFrame::getPositioningInfo() {
    const bool off = m_hidden || !cfg::enabled();
    const int  B   = std::max(0L, cfg::border());
    const int  T   = std::max(0L, cfg::titleHeight());

    SDecorationPositioningInfo info;
    info.policy         = off ? DECORATION_POSITION_ABSOLUTE : DECORATION_POSITION_STICKY;
    info.edges          = DECORATION_EDGE_TOP | DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT;
    info.priority       = 10001; // logo acima da borda nativa (10000): a moldura encosta na janela
    info.reserved       = true;
    info.desiredExtents = off ? SBoxExtents{{0, 0}, {0, 0}} : SBoxExtents{{(double)B, (double)(B + T)}, {(double)B, (double)B}};
    return info;
}

void CMotifFrame::onPositioningReply(const SDecorationPositioningReply& reply) {
    m_assigned = reply.assignedGeometry;
    m_textDirty = true;
}

CBox CMotifFrame::assignedBoxGlobal() {
    if (!validMapped(m_window))
        return {};

    CBox box = m_assigned;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP | DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT, m_window.lock()));

    const auto PWORKSPACE      = m_window->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !m_window->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();
    return box.translate(WORKSPACEOFFSET);
}

Vector2D CMotifFrame::cursorRelativeToFrame() {
    return g_pInputManager->getMouseCoordsInternal() - assignedBoxGlobal().pos();
}

motif::Layout CMotifFrame::currentLayout() {
    const auto b = assignedBoxGlobal();
    return motif::layout(b.w, b.h, (int)cfg::border(), (int)cfg::titleHeight());
}

std::string CMotifFrame::getDisplayName() {
    return "HyprMotif";
}

eDecorationType CMotifFrame::getDecorationType() {
    return DECORATION_CUSTOM;
}

// enrolada: por cima da janela, para tampar a faixa mínima que sobra do cliente
eDecorationLayer CMotifFrame::getDecorationLayer() {
    return m_shaded ? DECORATION_LAYER_OVER : DECORATION_LAYER_UNDER;
}

uint64_t CMotifFrame::getDecorationFlags() {
    return DECORATION_ALLOWS_MOUSE_INPUT | DECORATION_PART_OF_MAIN_WINDOW;
}

PHLWINDOW CMotifFrame::getOwner() {
    return m_window.lock();
}

// ---------- dano ----------

// só o anel da moldura: redesenhar a janela inteira a cada troca de foco seria desperdício
void CMotifFrame::damageEntire() {
    const auto b = assignedBoxGlobal();
    if (b.w < 1 || b.h < 1)
        return;
    const double B = cfg::border(), T = cfg::titleHeight();
    g_pHyprRenderer->damageBox(CBox{b.x, b.y, b.w, B + T});
    g_pHyprRenderer->damageBox(CBox{b.x, b.y + b.h - B, b.w, B});
    g_pHyprRenderer->damageBox(CBox{b.x, b.y + B + T, B, std::max(0.0, b.h - 2 * B - T)});
    g_pHyprRenderer->damageBox(CBox{b.x + b.w - B, b.y + B + T, B, std::max(0.0, b.h - 2 * B - T)});
}

void CMotifFrame::updateWindow(PHLWINDOW) {
    damageEntire(); // título mudou, etc.
}

void CMotifFrame::onFocusChanged() {
    const bool now = m_window.lock() == Desktop::focusState()->window();
    if (now != m_active)
        damageEntire(); // o draw atualiza m_active
}

void CMotifFrame::onConfigReloaded() {
    m_textDirty = true;
    g_pDecorationPositioner->repositionDeco(this);
}

void CMotifFrame::updateRules() {
    const auto PWINDOW    = m_window.lock();
    const auto prevHidden = m_hidden;

    m_hidden = false;
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->noFrameRuleIdx))
        m_hidden = truthy(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->noFrameRuleIdx)->effect);

    if (prevHidden != m_hidden)
        g_pDecorationPositioner->repositionDeco(this);
}

// ---------- desenho ----------

void CMotifFrame::draw(PHLMONITOR pMonitor, float const& a) {
    if (m_lastEnabled != (bool)cfg::enabled()) {
        m_lastEnabled = cfg::enabled();
        g_pDecorationPositioner->repositionDeco(this);
    }

    if (m_hidden || !validMapped(m_window) || !cfg::enabled())
        return;

    const auto PWINDOW = m_window.lock();
    if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault())
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CFramePassElement>(CFramePassElement::SFrameData{this, a}));
}

// texto do título (cairo/pango -> textura), centralizado na faixa entre os botões
void CMotifFrame::renderTitle(const Vector2D& size, float scale, const std::string& title) {
    const auto  col = CHyprColor{(uint64_t)(m_active ? cfg::colText() : cfg::colTextInactive())};

    cairo_surface_t* S  = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size.x, size.y);
    cairo_t*         CR = cairo_create(S);

    cairo_save(CR);
    cairo_set_operator(CR, CAIRO_OPERATOR_CLEAR);
    cairo_paint(CR);
    cairo_restore(CR);

    PangoLayout*          layout = pango_cairo_create_layout(CR);
    PangoFontDescription* fd     = pango_font_description_from_string(cfg::font().c_str());
    pango_font_description_set_size(fd, cfg::fontSize() * scale * PANGO_SCALE);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    pango_layout_set_text(layout, title.c_str(), -1);
    pango_layout_set_width(layout, (int)size.x * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    const auto align = cfg::titleAlign();
    pango_layout_set_alignment(layout, align == "left" ? PANGO_ALIGN_LEFT : align == "right" ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_CENTER);

    int lw, lh;
    pango_layout_get_size(layout, &lw, &lh);
    cairo_set_source_rgba(CR, col.r, col.g, col.b, col.a);
    cairo_move_to(CR, 0, std::round((size.y - lh / (double)PANGO_SCALE) / 2.0));
    pango_cairo_show_layout(CR, layout);
    g_object_unref(layout);

    cairo_surface_flush(S);

    // mesmo upload do hyprbars: cairo é BGRA, o swizzle troca R e B
    m_textTex->allocate();
    glBindTexture(GL_TEXTURE_2D, m_textTex->m_texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
#ifndef GLES2
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
#endif
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, cairo_image_surface_get_data(S));

    cairo_destroy(CR);
    cairo_surface_destroy(S);
}

void CMotifFrame::renderPass(PHLMONITOR pMonitor, float const& a) {
    const auto PWINDOW = m_window.lock();
    if (!PWINDOW || !pMonitor)
        return;

    const int B = std::max(0L, cfg::border());
    const int T = std::max(0L, cfg::titleHeight());

    m_active = PWINDOW == Desktop::focusState()->window();

    const auto   FRAME = assignedBoxGlobal();
    const double S     = pMonitor->m_scale;

    // origem da moldura em coordenadas do monitor
    CBox origin = {FRAME.x - pMonitor->m_position.x, FRAME.y - pMonitor->m_position.y, FRAME.w, FRAME.h};
    origin.translate(PWINDOW->m_floatingOffset);
    if (origin.w < 1 || origin.h < 1)
        return;

    const auto L = motif::layout(FRAME.w, FRAME.h, B, T);

    // retângulo lógico (relativo à moldura) -> caixa em px do monitor; arredonda as arestas, não o
    // tamanho, para peças vizinhas se encostarem sem fresta em escala fracionária
    auto px = [&](const Rect& r) {
        const double x0 = std::round((origin.x + r.x) * S), y0 = std::round((origin.y + r.y) * S);
        const double x1 = std::round((origin.x + r.x + r.w) * S), y1 = std::round((origin.y + r.y + r.h) * S);
        return CBox{x0, y0, x1 - x0, y1 - y0};
    };
    auto fill = [&](const Rect& r, CHyprColor c) {
        const CBox b = px(r);
        if (b.w < 1 || b.h < 1)
            return;
        c.a *= a;
        g_pHyprOpenGL->renderRect(b, c, {});
    };

    const CHyprColor face  = CHyprColor{(uint64_t)(m_active ? cfg::colActive() : cfg::colInactive())};
    // sombras do mwm: se não vierem da config (0), saem da face
    const auto       topCfg = m_active ? cfg::colActiveTop() : cfg::colInactiveTop();
    const auto       botCfg = m_active ? cfg::colActiveBottom() : cfg::colInactiveBottom();
    const CHyprColor light  = topCfg ? CHyprColor{(uint64_t)topCfg} : lighter(face);
    const CHyprColor dark   = botCfg ? CHyprColor{(uint64_t)botCfg} : darker(face);

    // bisel de `bw` px em volta de `r`: raised = claro em cima/esquerda, escuro embaixo/direita
    // (sunken inverte). As bordas escuras vão primeiro e as claras cobrem os cantos.
    auto bevel = [&](const Rect& r, double bw, bool raised) {
        const auto tl = raised ? light : dark, br = raised ? dark : light;
        fill({r.x, r.y + r.h - bw, r.w, bw}, br);
        fill({r.x + r.w - bw, r.y, bw, r.h}, br);
        fill({r.x, r.y, r.w - bw, bw}, tl);
        fill({r.x, r.y, bw, r.h - bw}, tl);
    };
    auto raised = [&](const Rect& r, double bw, bool isRaised = true) {
        fill(r, face);
        bevel(r, bw, isRaised);
    };

    // 1) anel da moldura (face) + bisel externo de 2 px
    fill({0, 0, FRAME.w, (double)(B + T)}, face);
    fill({0, FRAME.h - B, FRAME.w, (double)B}, face);
    fill({0, (double)(B + T), (double)B, L.client.h}, face);
    fill({FRAME.w - B, (double)(B + T), (double)B, L.client.h}, face);
    if (B >= 2)
        bevel({0, 0, FRAME.w, FRAME.h}, 2, true);

    if (m_shaded)
        fill(L.client, face);

    // 2) friso "afundado" em volta do cliente, dentro da borda
    if (B >= 3 && L.client.w > 0 && L.client.h > 0)
        bevel({L.client.x - 1, L.client.y - 1, L.client.w + 2, L.client.h + 2}, 1, false);

    // 3) barra de título
    if (T >= 2 && L.bar.w > 0)
        raised(L.bar, 1);

    // 4) botões: cada um é um quadrado em relevo (afundado enquanto apertado) com um glifo
    auto button = [&](const Rect& cell, Part which) {
        if (cell.w <= 0)
            return;
        const bool down = m_pressed == which;
        const Rect r    = cell.inset(1);
        raised(r, 1, !down);

        const double off = down ? 1 : 0; // glifo desce 1 px quando apertado
        const double cx = r.x + r.w / 2.0 + off, cy = r.y + r.h / 2.0 + off;
        auto glyph = [&](double gw, double gh) {
            gw = std::floor(gw), gh = std::floor(gh);
            raised({std::floor(cx - gw / 2), std::floor(cy - gh / 2), gw, gh}, 1);
        };
        switch (which) {
            case Part::Menu: glyph(std::max(6.0, r.w * 0.5), std::max(3.0, r.h * 0.2)); break; // traço
            case Part::Minimize: glyph(std::max(4.0, r.w * 0.3), std::max(4.0, r.w * 0.3)); break; // quadradinho
            case Part::Maximize: glyph(std::max(7.0, r.w * 0.6), std::max(7.0, r.w * 0.6)); break; // quadrado grande
            default: break;
        }
    };
    button(L.menu, Part::Menu);
    button(L.minimize, Part::Minimize);
    button(L.maximize, Part::Maximize);

    // 5) título, centralizado entre os botões
    // alinhamento: left/right ganham 4 px de folga na barra
    Rect textRect = L.text;
    const auto align = cfg::titleAlign();
    if (align == "left" || align == "right") {
        textRect.x += 4;
        textRect.w = std::max(0.0, textRect.w - 8);
    }
    if (cfg::titleEnabled() && textRect.w > 4) {
        const CBox  tb = px(textRect);
        const auto  title = PWINDOW->m_title;
        const Vector2D size{tb.w, tb.h};
        if (m_textDirty || title != m_lastTitle || size != m_lastTextSize || m_active != m_lastTextActive || m_textTex->m_texID == 0) {
            renderTitle(size, S, title);
            m_lastTitle      = title;
            m_lastTextSize   = size;
            m_lastTextActive = m_active;
            m_textDirty      = false;
        }
        g_pHyprOpenGL->renderTexture(m_textTex, tb, {.a = a});
    }
}

// ---------- entrada ----------

// mesmas condições do hyprbars: workspace visível, sem layer/lock por cima, cursor nesta janela
bool CMotifFrame::inputIsValid() {
    if (!cfg::enabled() || m_hidden)
        return false;

    if (!m_window->m_workspace || !m_window->m_workspace->isVisible() || !g_pInputManager->m_exclusiveLSes.empty() ||
        (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_window->wlSurface()->resource())))
        return false;

    const auto WINDOWATCURSOR = g_pCompositor->vectorToWindowUnified(g_pInputManager->getMouseCoordsInternal(),
                                                                     Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

    auto focusState = Desktop::focusState();
    auto window     = focusState->window();
    auto monitor    = focusState->monitor();

    if (WINDOWATCURSOR != m_window && m_window != window)
        return false;

    auto     PMONITOR     = monitor;
    PHLLS    foundSurface = nullptr;
    Vector2D surfaceCoords;

    g_pCompositor->vectorToLayerSurface(g_pInputManager->getMouseCoordsInternal(), &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &foundSurface);
    if (foundSurface)
        return false;
    g_pCompositor->vectorToLayerSurface(g_pInputManager->getMouseCoordsInternal(), &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords,
                                        &foundSurface);
    if (foundSurface)
        return false;

    return true;
}

void CMotifFrame::onMouseButton(SCallbackInfo& info, IPointer::SButtonEvent e) {
    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        // o release precisa ser tratado mesmo se o cursor saiu: encerra arraste e limpa botão apertado
        if (m_pressed != Part::None || m_dragging || m_dragPending || m_cancelledDown)
            handleUp(info);
        return;
    }

    if (e.button != BTN_LEFT_ || !inputIsValid())
        return;

    handleDown(info);
}

void CMotifFrame::handleDown(SCallbackInfo& info) {
    const auto PWINDOW = m_window.lock();
    const auto L       = currentLayout();
    const auto F       = assignedBoxGlobal();
    const auto C       = cursorRelativeToFrame();
    const auto PART    = motif::hit(L, F.w, F.h, C.x, C.y);

    if (PART == Part::None || PART == Part::Client)
        return; // clique no conteúdo da janela: não é da moldura

    if (Desktop::focusState()->window() != PWINDOW)
        Desktop::focusState()->fullWindowFocus(PWINDOW);
    if (PWINDOW->m_isFloating)
        g_pCompositor->changeWindowZOrder(PWINDOW, true);

    info.cancelled   = true;
    m_cancelledDown  = true;

    const bool dbl = m_lastDownPart == PART && std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastDown).count() < 400;
    m_lastDown     = Time::steadyNow();
    m_lastDownPart = PART;

    switch (PART) {
        case Part::Menu:
            // como no mwm: duplo clique no botão de menu fecha a janela
            if (dbl) {
                m_lastDownPart = Part::None;
                runDispatch("killactive");
            }
            return;
        case Part::Minimize:
        case Part::Maximize:
            m_pressed = PART; // a ação roda ao soltar, se o cursor continuar no botão
            damageEntire();
            return;
        case Part::Title:
            if (dbl) { // duplo clique na barra: `double_click` (maximize | shade | none)
                m_lastDownPart = Part::None;
                runDoubleClick();
                return;
            }
            [[fallthrough]];
        default: // Title / Frame: arraste (move / redimensiona) começa quando o mouse se mexe
            if (PART == Part::Frame && m_shaded)
                return; // enrolada: não há o que redimensionar
            m_dragKind    = PART;
            m_dragPending = true;
            return;
    }
}

void CMotifFrame::onMouseMove() {
    if (!m_dragPending || !validMapped(m_window))
        return;
    m_dragPending = false;
    m_dragging    = true;
    runDispatch(m_dragKind == Part::Title ? "mouse 1movewindow" : "mouse 1resizewindow");
}

void CMotifFrame::endDrag() {
    if (m_dragging)
        runDispatch(m_dragKind == Part::Title ? "mouse 0movewindow" : "mouse 0resizewindow");
    m_dragging    = false;
    m_dragPending = false;
    m_dragKind    = Part::None;
}

void CMotifFrame::handleUp(SCallbackInfo& info) {
    if (m_cancelledDown)
        info.cancelled = true;
    m_cancelledDown = false;

    endDrag();

    if (m_pressed != Part::None) {
        const auto pressed = m_pressed;
        m_pressed          = Part::None;
        damageEntire();

        const auto L = currentLayout();
        const auto F = assignedBoxGlobal();
        const auto C = cursorRelativeToFrame();
        if (motif::hit(L, F.w, F.h, C.x, C.y) == pressed)
            runAction(pressed);
    }
}

void CMotifFrame::runAction(Part p) {
    if (p == Part::Minimize)
        runDispatch(cfg::onMinimize());
    else if (p == Part::Maximize) {
        if (m_shaded)
            toggleShade(); // desenrola antes de maximizar
        runDispatch(cfg::onMaximize());
    }
}

void CMotifFrame::runDoubleClick() {
    const auto mode = cfg::doubleClick();
    if (mode == "shade")
        toggleShade();
    else if (mode == "maximize")
        runAction(Part::Maximize);
}

// O Hyprland não tem "enrolar": reduz a janela flutuante à altura mínima que ele aceitar (e o
// cliente pode impor um mínimo maior); a moldura, desenhada por cima, tampa a faixa que sobrar.
void CMotifFrame::toggleShade() {
    const auto PWINDOW = m_window.lock();
    if (!PWINDOW || !validMapped(m_window))
        return;

    const auto addr = std::format("address:0x{:x}", (uintptr_t)PWINDOW.get());
    g_pHyprRenderer->damageBox(assignedBoxGlobal());

    if (m_shaded) {
        m_shaded = false;
        runDispatch(std::format("resizewindowpixel exact {} {},{}", (int)m_savedSize.x, (int)m_savedSize.y, addr));
    } else {
        if (!PWINDOW->m_isFloating)
            return; // em tiled o layout manda no tamanho
        m_savedSize = PWINDOW->m_realSize->goal();
        m_shaded    = true;
        runDispatch(std::format("resizewindowpixel exact {} 1,{}", (int)m_savedSize.x, addr));
    }
    g_pHyprRenderer->damageBox(assignedBoxGlobal());
}
