# hyprtab

Alt+Tab estilo macOS para o Hyprland, em **um único plugin `.so`** (C++). Pensado para **0.52 até 0.57+**; só a 0.57 foi compilada até agora (veja [Status](#status)).

- Alt+Tab / Alt+Shift+Tab alternam entre as janelas por uso recente (MRU).
- Barra central com os ícones dos apps, desenhada pelo próprio compositor.
- Soltar o Alt confirma.

## Teclas e mouse (com o switcher aberto)

| Ação | Efeito |
|---|---|
| `←` / `→` | move a seleção |
| `Enter` ou soltar `Alt` | confirma |
| `Esc` | cancela |
| mover o mouse sobre um ícone | seleciona (só depois que o mouse realmente se mexe; a barra costuma abrir debaixo do cursor) |
| clique esquerdo num ícone | confirma aquele app |
| clique fora da barra | cancela |

## Instalação

Dependências de build: headers do Hyprland (`hyprland`), `pango`, `cairo`, `pixman`, `libdrm`. Opcional: `librsvg` (ícones SVG; detectado sozinho pelo `Makefile`).

```sh
make
```

Gera `hyprtab.so`. Sem `librsvg`, só ícones PNG funcionam; sem ícone aparece um círculo com a inicial do app.

### Config hyprlang (`hyprland.conf`, até 0.56)

```ini
plugin = /caminho/para/hyprtab.so
bind = ALT, TAB, hyprtab:next
bind = ALT SHIFT, TAB, hyprtab:prev
```

### Config Lua (`hyprland.lua`, 0.57+)

```lua
hl.plugin.load("/caminho/para/hyprtab.so")
hl.bind("ALT + TAB",         function() hl.plugin.hyprtab.next() end)
hl.bind("ALT + SHIFT + TAB", function() hl.plugin.hyprtab.prev() end)
```

Também funciona via `hyprctl plugin load /caminho/para/hyprtab.so` ou `hyprpm`.

## Compatibilidade por versão

A API de plugins do Hyprland mudou bastante entre 0.52 e 0.57. As diferenças são detectadas em tempo de compilação (`src/compat.hpp`) e isoladas em poucas funções.

| Hyprland   | Eventos                   | Dispatchers                   | Render                                |
|------------|---------------------------|-------------------------------|---------------------------------------|
| 0.52–0.53  | `registerCallbackDynamic` | `addDispatcherV2`             | `CTexture`, `renderRect(box, …)`      |
| 0.54       | `Event::bus()`            | `addDispatcherV2`             | `CTexture`, `renderRect(box, …)`      |
| 0.55–0.56  | `Event::bus()`            | `addDispatcherV2` (+ Lua)     | `ITexture` + `createTexture(cairo)`   |
| 0.57+      | `Event::bus()`            | `addLuaFunction` (config Lua) | `renderRect(ctx, box, …)`             |

Na 0.57+ `hyprctl dispatch` aceita expressões Lua; o foco tenta o formato legado e depois o Lua. Classe e título das janelas vêm de `hyprctl clients -j`, que é estável entre versões.

## Estrutura

- `src/main.cpp`: MRU, teclado, mouse, dispatchers e foco.
- `src/ui.cpp`: layout, desenho, cache de texturas e hit-test (a parte que muda por versão do renderer).
- `src/icons.cpp`: classe da janela → `.desktop` → arquivo de ícone → surface cairo (sem dependência do Hyprland).
- `src/compat.hpp`: detecção da era do Hyprland.

## Status

- [x] Compila contra os headers reais do Hyprland 0.57.2
- [ ] 0.52 – 0.56: o código tem ramos para elas (lidos dos headers), mas **nunca foram compilados**
- [ ] Testado rodando em um Hyprland de verdade (ainda não: desenvolvido no macOS)

## Licença

[MIT](LICENSE)
