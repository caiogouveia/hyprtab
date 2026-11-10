# hyprdock

Dock estilo macOS para o Hyprland, em **um único plugin `.so`** (C++), com o mesmo visual de vidro do [hyprtab](../README.md): fundo translúcido com blur, sombra e borda dupla.

- Embaixo, na esquerda ou na direita da tela (`position`).
- Reserva a faixa da dock, como uma barra: as janelas não ficam por baixo dela (`reserve`).
- Apps fixos pela config, seguidos dos apps abertos.
- Bolinha embaixo de cada app aberto.
- Zoom estilo macOS: os ícones perto do mouse crescem para cima, com animação de entrada e saída. O ícone sob o mouse não foge dele: a dock se abre para os lados a partir do cursor.
- Mouse em cima: tooltip com o nome do app (do `.desktop`).
- Clique: foca o app; clicando de novo, passa pelas janelas dele. Se não estiver aberto, abre (`Exec=` do `.desktop`).
- Some quando o workspace tem janela em tela cheia.
- Botão direito num app: menu com **Fechar** (todas as janelas do app), **Maximizar**, **Minimizar** e **Fixar na Dock / Remover da Dock**. Se o app não está aberto: **Abrir** e fixar/remover. `Esc`, um clique fora ou o botão direito de novo fecham o menu.

### Minimizar

O Hyprland não tem minimizar: a janela vai para o workspace especial `minimized`, escondido. Clicar no app na dock traz ela de volta para o workspace atual.

### Fixar pelo menu

O que você fixa ou remove pelo menu fica salvo em `~/.local/state/hyprdock/pins` (ou `$XDG_STATE_HOME/hyprdock/pins`), por cima da lista `apps` da config: uma linha `+classe` para cada app fixado e `-classe` para cada um removido. Apagar o arquivo volta para exatamente o que está no `hyprland.conf`.

> Primeira versão: testada no Hyprland 0.53.3. A reserva de espaço funciona de 0.53 a 0.56; na 0.57 o layout mudou e a dock fica por cima das janelas.

## Instalação

Mesmas dependências do hyprtab. Na pasta `dock/`:

```sh
make
```

Gera `dock/hyprdock.so`.

## Config (`hyprland.conf`, até 0.56)

```ini
plugin = /caminho/para/hyprtab/dock/hyprdock.so

plugin {
    hyprdock {
        apps = org.gnome.Nautilus, google-chrome, Alacritty   # fixos: classe da janela ou nome do .desktop
        icon_size = 48                                         # px
        margin = 8                                             # distância da borda da tela, px
        show_running = 1                                       # 0: só os fixos
        magnification = 1.6                                    # zoom do ícone sob o mouse; 1 desliga
        position = bottom                                      # bottom, left ou right
        reserve = 1                                            # 0: a dock fica por cima das janelas
    }
}
```

Para descobrir a classe de um app aberto: `hyprctl clients | grep class`.

Na 0.57+ (config em Lua) as opções ainda não são lidas e a dock usa os valores padrão.

## Estrutura

- `src/main.cpp`: plugin (config, mouse, desenho, abrir/focar apps).
- `src/model.cpp`: itens, geometria e hit-test, sem dependência do Hyprland.
- `tests/test.cpp`: testes do modelo: `make test`.
- Código comum com o hyprtab em `../src`: `render.cpp` (painel de vidro e render por versão), `icons.cpp`, `compat.hpp`, `clients.cpp`.
