#pragma once

#include "model.hpp"

#include <cstdint>
#include <string>

struct wl_event_loop;

// Menus exportados pelos apps via D-Bus (sd-bus, assíncrono, integrado ao event loop do compositor).
//
// O plugin é o `com.canonical.AppMenu.Registrar`: apps X11 (Chrome/Electron com --ozone-platform=x11,
// Qt com QT_QPA_PLATFORM=xcb, Firefox X11...) chamam RegisterWindow(xid, caminho) e publicam o menu em
// `com.canonical.dbusmenu`. A barra lê o menu da janela em foco e manda os cliques de volta.
//
// Nada aqui bloqueia: toda chamada é assíncrona e a resposta chega pelo event loop.
namespace appmenu {

bool start(wl_event_loop* loop, void (*onChange)());
void stop();

// menu da janela X11 `xid` (raiz: os filhos são os menus da barra). Troca o app acompanhado quando o
// xid muda e começa a carregar; nullptr enquanto não carregou ou se o app não registrou menu.
const mb::Node* menuFor(uint32_t xid);

// menu `id` vai abrir: o app pode preencher os filhos agora (Chrome carrega sob demanda)
void opened(int id);
void closed(int id);
// clique no item `id`
void activate(int id);

// "registrar: sim, 3 janelas" (para o hyprctl)
std::string status();

} // namespace appmenu
