#pragma once

#include "model.hpp"

#include <string>
#include <vector>

struct wl_event_loop;

// Volume da saída de áudio padrão pelo `wpctl` (PipeWire/WirePlumber).
//
// Nada aqui bloqueia: cada comando roda num processo filho e a saída chega por um pipe vigiado pelo
// event loop do compositor. Só um comando de cada tipo por vez: leituras não se acumulam se o PipeWire
// travar, e mudanças seguidas (arrastar o slider) mandam só o valor mais recente.
namespace volume {

struct State {
    bool                  ok = false; // o wpctl respondeu (sem ele, a barra não mostra o volume)
    double                vol   = 0;  // 1.0 = 100%
    bool                  muted = false;
    std::vector<mb::Sink> sinks;      // saídas (lidas quando o menu abre)
};

bool start(wl_event_loop* loop, void (*onChange)());
void stop();

const State& state();

// relê o volume (e a lista de saídas, se `sinks`)
void refresh(bool sinks = false);

// muda já no estado local (a barra responde na hora) e manda para o PipeWire
void setVolume(double v);
void toggleMute();
void setDefault(int sinkId);

// para o hyprctl
std::string status();

} // namespace volume
