# hyprpager

Pager de workspaces para o Hyprland no estilo dos temas metálicos clássicos do Enlightenment, em **um único plugin `.so`** (C++). Desenho feito do zero, em código (cairo), sem imagens de tema.

- Uma coluna de telas de TV antigas, uma por workspace (1 a N), em cromo com reflexo azul-esverdeado e com volume: canos cilíndricos, caixa de TV com bisel, vidro afundado e abaulado, sombras no fundo.
- Ao lado de cada tela, um **refletor** pendurado num cano. O do workspace atual gira até a tela, acende e a ilumina; ao trocar de workspace, o antigo gira de volta e apaga, e o novo gira e acende (~0,4 s).
- Dentro de cada tela, as janelas daquele workspace na posição e proporção reais: retângulo esverdeado com o ícone do app. A janela em foco tem a borda mais clara.
- Fica do lado de dentro da área reservada pelas barras e pela dock (não fica por cima delas) e some quando o workspace tem janela em tela cheia.

## Mouse

| Ação | Efeito |
|---|---|
| clique numa tela | vai para o workspace |
| clique numa janela dentro da tela | vai para o workspace e foca a janela |
| arrastar uma janela para outra tela | muda a janela de workspace, sem sair do atual |
| `Esc` arrastando | cancela |

Cliques nos vãos entre as telas passam para a janela de baixo.

## Instalação

Mesmas dependências do hyprtab. Na pasta `pager/`:

```sh
make
```

Gera `pager/hyprpager.so`.

## Config (`hyprland.conf`, até 0.56)

```ini
plugin = /caminho/para/hyprtab/pager/hyprpager.so

plugin {
    hyprpager {
        workspaces = 4      # quantas telas (workspaces 1..N), de 1 a 20
        size = 64           # largura de cada tela, px
        position = left     # left ou right
        align = center      # top, center ou bottom
        offset = 0          # deslocamento vertical depois de alinhar, px (positivo desce)
        margin = 12         # distância da borda (ou da área reservada pela dock/barras), px
        monitor =           # em quais monitores aparece (ex.: DP-1, HDMI-A-1); vazio: todos
    }
}
```

Com `align = top` ou `bottom`, a `margin` também vale para o topo ou a base. O pager nunca sai da tela, mesmo com `offset` grande. Mudanças valem no `hyprctl reload`.

## Config Lua (0.57+)

As mesmas opções, com `hl.config`:

```lua
hl.config({
    plugin = {
        hyprpager = {
            workspaces = 6,
            position = "right",
            align = "top",
            offset = 40,
            monitor = "DP-1",
        },
    },
})
```

> Primeira versão: testada no Hyprland 0.53.3.

## Estrutura

- `src/main.cpp`: plugin (config, janelas, desenho, mouse, trocar de workspace e mover janelas).
- `src/chrome.cpp`: o desenho em cairo, em camadas: base (canos, caixas, vidros) por baixo das janelas; luz e reflexos por cima; e o refletor, pré-desenhado em 41 pontos da rotação (a animação só escolhe a textura).
- `src/model.cpp`: layout, encaixe do monitor no vidro, hit-test, sem dependência do Hyprland.
- `tests/test.cpp`: testes do modelo: `make test`.
- Código comum com o hyprtab em `../src`: `render.cpp`, `icons.cpp`, `compat.hpp`.
