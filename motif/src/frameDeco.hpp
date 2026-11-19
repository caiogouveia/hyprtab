#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include "globals.hpp"
#include "geometry.hpp"

#define private public
#include <hyprland/src/managers/input/InputManager.hpp>
#undef private

#include <optional>
#include <string>

// Moldura Motif: uma decoração só, nas quatro bordas (como a borda nativa do Hyprland), com barra de
// título no topo. Tudo é retângulo chapado (bisel claro/escuro), exceto o texto do título.
class CMotifFrame : public IHyprWindowDecoration {
  public:
    CMotifFrame(PHLWINDOW);
    virtual ~CMotifFrame();

    virtual SDecorationPositioningInfo getPositioningInfo();
    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply);
    virtual void                       draw(PHLMONITOR, float const& a);
    virtual eDecorationType            getDecorationType();
    virtual void                       updateWindow(PHLWINDOW);
    virtual void                       damageEntire();
    virtual eDecorationLayer           getDecorationLayer();
    virtual uint64_t                   getDecorationFlags();
    virtual std::string                getDisplayName();

    PHLWINDOW                          getOwner();
    void                               updateRules();
    void                               onConfigReloaded();
    void                               onFocusChanged();
    void                               toggleShade(); // enrola/desenrola (só janelas flutuantes)

    WP<CMotifFrame>                    m_self;

  private:
    PHLWINDOWREF m_window;
    CBox         m_assigned; // relativo ao ponto das bordas (ver assignedBoxGlobal)

    bool         m_hidden = false;
    bool         m_lastEnabled = false;
    bool         m_active = false;

    // shade: a janela é reduzida à altura mínima e a moldura cobre o resto
    bool         m_shaded = false;
    Vector2D     m_savedSize;

    // texto do título: só é refeito quando algo muda
    SP<CTexture> m_textTex;
    std::string  m_lastTitle;
    Vector2D     m_lastTextSize;
    bool         m_lastTextActive = false;
    bool         m_textDirty      = true;

    // interação
    motif::Part                m_pressed = motif::Part::None; // botão apertado (solta dentro dele = ação)
    motif::Part                m_dragKind = motif::Part::None; // Title = mover, Frame = redimensionar
    bool                       m_dragPending = false;
    bool                       m_dragging = false;
    bool                       m_cancelledDown = false;
    Time::steady_tp            m_lastDown = Time::steadyNow();
    motif::Part                m_lastDownPart = motif::Part::None;

    SP<HOOK_CALLBACK_FN>       m_mouseButtonCb;
    SP<HOOK_CALLBACK_FN>       m_mouseMoveCb;

    CBox                       assignedBoxGlobal();
    Vector2D                   cursorRelativeToFrame();
    motif::Layout              currentLayout();
    void                       renderPass(PHLMONITOR, float const& a);
    void                       renderTitle(const Vector2D& size, float scale, const std::string& title);
    bool                       inputIsValid();
    void                       onMouseButton(SCallbackInfo& info, IPointer::SButtonEvent e);
    void                       onMouseMove();
    void                       handleDown(SCallbackInfo& info);
    void                       handleUp(SCallbackInfo& info);
    void                       runAction(motif::Part p);
    void                       runDoubleClick();
    void                       endDrag();

    friend class CFramePassElement;
};
