# hyprdock

Dock inspirada na do macOS para o Hyprland, em **um único plugin `.so`** (C++), com o mesmo visual de vidro do [hyprtab](../README.md): fundo translúcido com blur, sombra e borda dupla.

- Embaixo, na esquerda ou na direita da tela (`position`).
- Entra deslizando da borda ao iniciar (e ao trocar de borda).
- Reserva a faixa da dock, como uma barra: as janelas não ficam por baixo dela (`reserve`).
- Apps fixos pela config, seguidos dos apps abertos, com um separador entre os dois grupos (como no macOS; só aparece quando há apps abertos não fixos).
- **Lixeira** no fim, depois de um separador, com o ícone do tema (vazia ou cheia, conforme `~/.local/share/Trash/files`). Clique abre a lixeira no gerenciador de arquivos (pelo serviço `org.freedesktop.FileManager1`, não pelo app padrão de pastas); o botão direito tem **Abrir** e **Esvaziar Lixeira…**, que pede confirmação no próprio menu antes de apagar tudo para sempre (`gio trash --empty`).
- Bolinha embaixo de cada app aberto.
- Ampliação (como no macOS): os ícones perto do mouse crescem (também com o cursor encostado na borda da tela, na margem entre a dock e a borda) para cima, com animação de entrada e saída. O ícone sob o mouse não foge dele: a dock se abre para os lados a partir do cursor.
- Mouse em cima: tooltip com o nome do app (do `.desktop`).
- Clique: foca o app; clicando de novo, passa pelas janelas dele. Se não estiver aberto, abre (`Exec=` do `.desktop`).
- O ícone pula enquanto o app abre pela dock, e duas vezes quando um app aberto por fora (lançador, terminal) aparece.
- Some quando o workspace tem janela em tela cheia.
- Arrastar um app fixo reordena os fixos (a ordem fica salva). Arrastar para longe da dock e soltar remove dos fixos (aberto, ele continua na dock como app aberto). `Esc` cancela no meio do arraste.
- Botão direito num app: menu com **Fechar** (todas as janelas do app), **Maximizar**, **Minimizar** e **Fixar na Dock / Remover da Dock**. Se o app não está aberto: **Abrir** e fixar/remover. `Esc`, um clique fora ou o botão direito de novo fecham o menu.
- Arrastar o separador para dentro da tela aumenta a dock, e para a borda diminui (ícones de 16 a 128 px), como no macOS. `Esc` no meio do arraste volta ao tamanho de antes. Em cima do separador o cursor vira o de redimensionar. O separador antes da Lixeira sempre existe, então dá sempre para redimensionar.
- Botão direito no separador ou numa parte vazia da dock: menu da dock com **Ampliação** (ligar/desligar e um controle deslizante de 1,1× a 2,5×, como nos ajustes da dock do macOS), **Reservar Espaço na Tela**, **Mostrar Apps Abertos** e **Posição na Tela ›**, um submenu com Esquerda, Embaixo e Direita. O dispatcher `hyprdock:menu` abre o mesmo menu (`bind = SUPER, D, hyprdock:menu`).

### Minimizar

O Hyprland não tem minimizar: a janela vai para o workspace especial `minimized`, escondido. Clicar no app na dock traz ela de volta para o workspace atual.

### Ajustes pelo menu da dock

O que você muda pelo menu da dock fica salvo em `~/.local/state/hyprdock/settings` (ou `$XDG_STATE_HOME/hyprdock/settings`), por cima do `hyprland.conf`: uma linha `chave=valor` para cada ajuste mudado (`position`, `magnification`, `magnification_level`, `reserve`, `show_running`, `icon_size`). Apagar o arquivo volta para o que está na config. O valor da ampliação começa no `magnification` da config (ou 1,6 se lá estiver 1, que é desligada) e passa a ser o do controle depois que você mexe nele.

### Fixar pelo menu

O que você fixa ou remove pelo menu fica salvo em `~/.local/state/hyprdock/pins` (ou `$XDG_STATE_HOME/hyprdock/pins`), por cima da lista `apps` da config: uma linha `+classe` para cada app fixado, `-classe` para cada um removido e `=classe` com a ordem definida arrastando. Apagar o arquivo volta para exatamente o que está no `hyprland.conf`.

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
        ignore = steam_app_*, foo                              # classes que não aparecem como abertas; `*` no começo/fim
    }
}
```

Para descobrir a classe de um app aberto: `hyprctl clients | grep class`.

Janelas auxiliares já são ignoradas por padrão (seletor de compartilhar tela, portais `xdg-desktop-portal*`, agentes de polkit, diálogos do Hyprland); `ignore` acrescenta outras. Isso vale para janelas abertas: um app que você fixou continua aparecendo.

Na 0.57+ (config em Lua) as opções ainda não são lidas e a dock usa os valores padrão.

## Estrutura

- `src/main.cpp`: plugin (config, mouse, desenho, abrir/focar apps).
- `src/model.cpp`: itens, geometria e hit-test, sem dependência do Hyprland.
- `tests/test.cpp`: testes do modelo: `make test`.
- Código comum com o hyprtab em `../src`: `render.cpp` (painel de vidro e render por versão), `icons.cpp`, `compat.hpp`, `clients.cpp`.
