# hyprmotif

Molduras estilo **Motif/mwm** para o Hyprland, em **um único plugin `.so`** (C++). Funciona como o `hyprbars` (decoração de janela com barra de título), mas com o visual do Motif: borda em relevo, friso afundado em volta do conteúdo, botão de menu à esquerda e minimizar/maximizar à direita, título centralizado em negrito.

> **Primeira versão, ainda não compilada nem testada num Hyprland de verdade.** Foi escrita contra a API do Hyprland **0.53** (baseada no `hyprbars` v0.53.0 e na borda nativa do Hyprland), a mesma do seu setup. Não há ramo para 0.54+ ainda, e o CI não compila esta pasta por enquanto.

## Instalação

Mesmas dependências do hyprtab. Na pasta `motif/`:

```sh
make
```

Gera `motif/hyprmotif.so`. `make test` roda os testes da geometria (sem Hyprland).

## Config (`hyprland.conf`)

```ini
plugin = /caminho/para/motif/hyprmotif.so

# a moldura substitui a borda, o arredondamento e a sombra nativos
general {
    border_size = 0
}
decoration {
    rounding = 0
    shadow { enabled = false }
}

plugin {
    hyprmotif {
        enabled      = true
        border_size  = 4            # borda em volta da janela (px)
        title_height = 18           # altura da barra de título (px)

        # título
        title_enabled = true
        title_align   = center      # left | center | right
        font          = Sans Bold
        font_size     = 10

        # cores, como os recursos do mwm (janela em foco / sem foco)
        col.active                 = rgba(aeb2c3ff)   # activeBackground
        col.text                   = rgba(000000ff)   # activeForeground
        col.active_top_shadow      = rgba(e3e5ebff)   # activeTopShadowColor    (opcional)
        col.active_bottom_shadow   = rgba(51535aff)   # activeBottomShadowColor (opcional)
        col.inactive               = rgba(bcbcbcff)   # background
        col.text_inactive          = rgba(404040ff)   # foreground
        col.inactive_top_shadow    = rgba(f0f0f0ff)   # topShadowColor          (opcional)
        col.inactive_bottom_shadow = rgba(545454ff)   # bottomShadowColor       (opcional)

        double_click = maximize     # duplo clique na barra: maximize | shade | none
        on_minimize  = movetoworkspacesilent special:minimized
        on_maximize  = fullscreen 1
    }
}
```

As sombras são opcionais: sem elas, o bisel claro e o escuro saem da cor da face (como o mwm faz). Os valores acima são só exemplos; omitir as quatro linhas `*_shadow` dá o visual padrão.

`on_minimize` e `on_maximize` são dispatchers do Hyprland (`nome argumentos`) executados na janela ativa. O Hyprland não tem minimizar: a janela vai para o workspace especial, e você a traz de volta com, por exemplo, `bind = SUPER, M, togglespecialworkspace, minimized`.

Para tirar a moldura de uma janela (diálogos, popups, janelas que já trazem a própria barra):

```ini
windowrule = match:class ^(minha-classe)$, hyprmotif:no_frame on
```

## Mouse

| Ação | Efeito |
|---|---|
| arrastar a barra de título | move a janela |
| arrastar a borda | redimensiona (pelo canto/lado mais próximo, como o `resizewindow`) |
| clicar e soltar em minimizar / maximizar | executa `on_minimize` / `on_maximize` (o botão afunda enquanto apertado; soltar fora cancela) |
| duplo clique no botão de menu | fecha a janela (como no mwm) |
| duplo clique na barra | `double_click`: maximiza (padrão), enrola (`shade`) ou nada |

### Enrolar a janela (shade)

Recolhe a janela para a barra de título e desenrola de volta ao tamanho anterior. Pelo duplo clique (`double_click = shade`) ou por atalho:

```ini
bind = SUPER, S, hyprmotif:shade
```

O Hyprland não tem isso nativamente, então é emulado: a janela **flutuante** é redimensionada para a menor altura que o Hyprland (e o app) aceitam, e a moldura, desenhada por cima, tampa o que sobrar. Janelas em tiling não enrolam. Enrolada, a borda não redimensiona; maximizar desenrola antes.

O botão de menu ainda não abre o menu de janela do mwm.

## Estrutura

- `src/geometry.cpp`: layout da moldura e hit-test em px lógicos (sem dependência do Hyprland).
- `src/frameDeco.cpp`: a decoração (`IHyprWindowDecoration` nas quatro bordas, como a borda nativa): posicionamento, desenho com retângulos chapados e entrada do mouse.
- `src/FramePassElement.cpp`: elemento da render pass (o GL só pode rodar dentro da pass).
- `src/globals.hpp`: estado global e acesso à config.
- `src/main.cpp`: init, eventos e opções.
- `tests/test.cpp`: testes da geometria.

## Licença

[MIT](../LICENSE)
