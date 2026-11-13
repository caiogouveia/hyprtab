#include "FramePassElement.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include "frameDeco.hpp"

CFramePassElement::CFramePassElement(const CFramePassElement::SFrameData& data_) : data(data_) {
    ;
}

void CFramePassElement::draw(const CRegion& damage) {
    data.deco->renderPass(g_pHyprOpenGL->m_renderData.pMonitor.lock(), data.a);
}

bool CFramePassElement::needsLiveBlur() {
    return false; // a moldura é opaca
}

bool CFramePassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> CFramePassElement::boundingBox() {
    // mesma folga do hyprbars: sem ela a oclusão fica agressiva demais
    return data.deco->assignedBoxGlobal().translate(-g_pHyprOpenGL->m_renderData.pMonitor->m_position).expand(10);
}
