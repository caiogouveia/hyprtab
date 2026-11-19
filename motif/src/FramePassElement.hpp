#pragma once
#include <hyprland/src/render/pass/PassElement.hpp>

class CMotifFrame;

// Elemento da render pass: o Hyprland só executa o desenho depois de montar a pass, então a
// decoração não pode chamar o GL direto dentro de draw() (ficaria por baixo das janelas).
class CFramePassElement : public IPassElement {
  public:
    struct SFrameData {
        CMotifFrame* deco = nullptr;
        float        a    = 1.F;
    };

    CFramePassElement(const SFrameData& data_);
    virtual ~CFramePassElement() = default;

    virtual void                draw(const CRegion& damage);
    virtual bool                needsLiveBlur();
    virtual bool                needsPrecomputeBlur();
    virtual std::optional<CBox> boundingBox();

    virtual const char*         passName() {
        return "CFramePassElement";
    }

  private:
    SFrameData data;
};
