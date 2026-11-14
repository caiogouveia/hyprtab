# hyprmenu

Barra de menus global inspirada na do macOS para o Hyprland, em **um único plugin `.so`** (C++), com o mesmo visual de vidro do [hyprtab](../README.md) e da [hyprdock](../dock/README.md). Sem GTK e sem Python: o D-Bus é feito com o `sd-bus` da libsystemd, integrado ao event loop do compositor e sempre assíncrono.

- Barra no topo de cada monitor, translúcida com blur. Reserva a faixa, então janelas e outras barras (waybar) ficam abaixo dela (`reserve`).
- **Logo** (menu do sistema): Bloquear Tela, Suspender, Reiniciar, Desligar, Recarregar Configuração, Encerrar Sessão.
- **Nome do app em foco**, em negrito (menu da janela): Nova Janela (pelo `.desktop`), Tela Cheia, Maximizar, Flutuante, Em Todos os Workspaces, Mover para Workspace ▸, Fechar Janela, Encerrar App.
- **Menus do app** (Arquivo, Editar, Exibir...), de um destes dois jeitos (veja abaixo):
  - **menu real**, quando o app exporta o menu: itens, atalhos, desabilitados e marcas vêm do app, e o clique é executado por ele;
  - **menu de atalhos**, para apps Wayland que não exportam: menus escritos num JSON por app, e o clique manda o atalho do app para a janela.
- **Volume** à direita, antes do relógio (precisa do `wpctl`, do WirePlumber): o ícone mostra o nível e o mudo. Clique abre um menu com slider, Sem Som, a lista de saídas (para trocar o dispositivo) e Configurações de Som…; rolar sobre o ícone muda 5% por clique da roda.
- Relógio à direita.
- Some quando o workspace tem janela em tela cheia.

## Mouse e teclado

| Ação | Efeito |
|---|---|
| clique num título | abre o menu; clicar de novo fecha |
| com um menu aberto, passar por outro título | troca de menu |
| pressionar num título, arrastar até o item e soltar | executa o item |
| clique num item | executa |
| passar sobre um item com `›` | abre o submenu |
| clique fora / `Esc` | fecha (`Esc` num submenu fecha só ele) |
| `↑` / `↓` | move a seleção |
| `→` / `←` | abre/fecha submenu; no nível de cima, troca de menu |
| `Enter` / `Espaço` | executa |
| no slider de volume: arrastar, ou `←` / `→` | muda o volume (5% por tecla) |
| rolar sobre o ícone de volume | ±5% por clique da roda |

Pelo teclado: o dispatcher `hyprmenu:open` abre o menu do app (`hyprmenu:open N` abre o N-ésimo título, 0 = logo). Chamar de novo fecha.

```ini
bind = ALT, F10, hyprmenu:open
```

Com o menu aberto, o teclado vai para o menu, não para o app.

## Quais apps mostram o menu

O menu real tem prioridade; sem ele, vale o menu de atalhos do app, se houver um mapa para ele.

### Menu real (o app exporta)

O plugin é o `com.canonical.AppMenu.Registrar`: os apps registram a janela e publicam o menu em `com.canonical.dbusmenu`. Isso funciona para janelas **X11** (XWayland):

| App | Como |
|---|---|
| Chrome / Chromium | `--ozone-platform=x11` |
| VS Code | `--ozone-platform=x11` e, no `settings.json`, `"window.titleBarStyle": "native"` + `"window.menuStyle": "native"` (testado no 1.146) |
| Outros Electron (Discord, Signal...) | `--ozone-platform=x11` |
| Qt 5/6 | `QT_QPA_PLATFORM=xcb` e o plugin de appmenu do Qt instalado |
| Firefox | `MOZ_ENABLE_WAYLAND=0` e `widget.gtk.global-menu.enabled = true` em `about:config` |

Abra o app depois de carregar o plugin. Se o plugin for recarregado, Chrome e Electron registram o menu de novo sozinhos.

Ainda faltam o protocolo `org_kde_kwin_appmenu` (Qt e Firefox no Wayland) e o `gtk_shell1` (apps GTK que têm barra de menu).

### Menu de atalhos (qualquer app, inclusive Wayland)

Para apps que não exportam menu, um arquivo JSON descreve os menus com os atalhos de teclado do próprio app. Clicar no item manda o atalho para a janela pelo dispatcher `sendshortcut` do Hyprland. O menu é fixo: não sabe se um item está disponível no momento e não mostra marcas.

Vêm com o plugin, escritos a partir da documentação de atalhos de cada app, em [`apps/`](apps):

| Mapa | Classes da janela |
|---|---|
| `vscode.json` | VS Code, Code - OSS, VSCodium |
| `chrome.json` | Google Chrome, Chromium |
| `nautilus.json` | Arquivos (Nautilus) |
| `alacritty.json` | Alacritty |

Os mapas são procurados em `~/.config/hyprmenu/apps/` (os seus; um arquivo com o mesmo nome substitui o que vem com o plugin) e na pasta `apps/` ao lado do `.so`. São relidos sozinhos quando um arquivo muda; erros aparecem no `hyprctl hyprmenu`.

```json
{
  "name": "Meu App",
  "match": ["meu-app", "org.exemplo.MeuApp"],
  "menus": [
    {"label": "Arquivo", "items": [
      {"label": "Salvar", "keys": "Ctrl+S"},
      {"label": "Abrir Pasta…", "keys": "Ctrl+K Ctrl+O"},
      "-",
      {"label": "Terminal", "keys": "Ctrl+`", "send": "Ctrl+code:49"},
      {"label": "Preferências", "items": [
        {"label": "Configurações", "keys": "Ctrl+,"}
      ]},
      {"label": "Salvar Tudo"}
    ]}
  ]
}
```

- `match`: classes da janela (`hyprctl clients | grep class`), sem diferenciar maiúsculas.
- `keys`: o atalho mostrado e enviado. Modificadores `Ctrl`, `Shift`, `Alt`, `Super`; teclas como `A`, `1`, `F5`, `Enter`, `Esc`, `Del`, `Tab`, `Space`, `Up`/`Down`/`Left`/`Right`, `Home`, `End`, `PgUp`, `PgDn` e pontuação (`,` `.` `/` `-` `=` `` ` `` `;` `'` `[` `]` `\`). Acordes em sequência separados por espaço.
- `send`: o que mandar, quando é diferente do que se mostra. Aceita `code:N` (código xkb da tecla física). Serve para apps que reconhecem atalhos pela posição da tecla, como o VS Code: no teclado ABNT2, o `` ` `` e o `/` do VS Code são as teclas físicas `code:49` e `code:61`.
- `"-"`: separador. `items`: submenu. Item sem `keys`: aparece desabilitado.

A tecla é procurada no layout de teclado ativo: uma tecla que só existe com Shift no layout (como `+` no ABNT2) não é encontrada. Prefira a tecla sem Shift (`Ctrl+=` em vez de `Ctrl++`).

Na 0.57+ (config em Lua), o nome do dispatcher usado ainda não foi conferido numa instalação real.

## Instalação

Mesmas dependências do hyprtab, mais `libsystemd` (já vem em qualquer sistema com systemd; no Ubuntu/Debian os headers estão em `libsystemd-dev`). Na pasta `menubar/`:

```sh
make
```

Gera `menubar/hyprmenu.so`.

## Config (`hyprland.conf`, até 0.56)

```ini
plugin = /caminho/para/hyprtab/menubar/hyprmenu.so

plugin {
    hyprmenu {
        height = 28                # altura da barra, px
        font_size = 13             # px
        reserve = 1                # 0: a barra fica por cima das janelas
        clock = 1                  # relógio à direita
        volume = 1                 # ícone e menu de volume (precisa do wpctl)
        sound_command =            # "Configurações de Som…"; vazio usa o primeiro instalado: pavucontrol, pwvucontrol, gnome-control-center (aberto com `XDG_CURRENT_DESKTOP=GNOME`, senão ele se recusa a rodar fora do GNOME)
        shadow = 1                 # sombra curta embaixo da barra (como no OS X até o Mavericks)
        lock_command = hyprlock    # comando do "Bloquear Tela"
        logo =                     # caminho de um PNG/SVG; vazio usa a orquídea desenhada
    }
}
```

Na 0.57+ (config em Lua) as opções ainda não são lidas e a barra usa os valores padrão. Para abrir pelo teclado: `hl.bind("ALT + F10", function() hl.plugin.hyprmenu.open() end)`.

> Primeira versão: testada no Hyprland 0.53.3. A reserva de espaço funciona de 0.53 a 0.56; na 0.57 a barra fica por cima das janelas.

## Diagnóstico

```sh
hyprctl hyprmenu
```

Mostra o volume lido e o último erro do `wpctl`, se o plugin é o Registrar, quantas janelas registraram menu, qual app está sendo acompanhado, o último erro do D-Bus e os mapas de atalhos carregados (com as pastas e os erros de cada arquivo).

## Estrutura

- `src/main.cpp`: plugin (config, barra, menus suspensos, mouse e teclado, ações, reserva de espaço).
- `src/volume.cpp`: volume pelo `wpctl`, em processos filhos lidos pelo event loop (nunca bloqueia; arrastar o slider manda só o valor mais recente).
- `src/appmenu.cpp`: D-Bus (Registrar, leitura do `com.canonical.dbusmenu`, cliques).
- `src/model.cpp`: árvore de menus, geometria, hit-test, JSON e mapas de atalhos, sem dependência do Hyprland.
- `apps/`: mapas de atalhos que vêm com o plugin.
- `tests/test.cpp`: testes do modelo: `make test`.
- Código comum com o hyprtab em `../src`: `render.cpp`, `icons.cpp`, `compat.hpp`.
