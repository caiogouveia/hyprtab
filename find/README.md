# hyprfind

Busca rápida inspirada no Spotlight do macOS para o Hyprland, em **um único plugin `.so`** (C++). Um atalho abre uma caixa de busca no meio da tela, desenhada pelo próprio compositor com o mesmo vidro do hyprtab e da dock. Enquanto ela está aberta, o teclado é dela.

Os resultados vêm em seções, nesta ordem:

| Seção | O que procura | `Enter` |
|---|---|---|
| **Calculadora** | o texto é uma conta: `2+3*4`, `1,5 × 4`, `200*15%`, `2^10`, `sqrt(2)`, `sen(pi/2)` | copia o resultado (`wl-copy`) |
| **Aplicativos** | `.desktop` instalados: nome no seu idioma, nome em inglês, `GenericName` e `Keywords` | abre o app (`Terminal=true` vai pelo `xdg-terminal-exec`) |
| **Janelas abertas** | título, nome do app e classe | foca a janela |
| **Arquivos** | arquivos e pastas do `~` | abre com `xdg-open`; `Ctrl+Enter` abre a pasta onde está |

A busca é fuzzy, como no fzf: `vsc` acha *Visual Studio Code*, `config` acha *Configurações* (sem precisar de acento) e várias palavras precisam casar todas. Os apps que você abre pelo hyprfind sobem na lista com o uso (contagem em `~/.local/state/hyprfind/usage`).

## Arquivos e fzf

A lista de arquivos vem de uma varredura do `~` feita pelo próprio plugin, numa thread separada (não trava o compositor). Pastas ocultas (`.algo`) e pastas que só têm coisa gerada (`node_modules`, `__pycache__`, `venv`, `target`...) ficam de fora. A lista fica em memória e é refeita, em segundo plano, quando a caixa abre e ela tem mais de 90 s.

Depois de uma pausa curta na digitação (120 ms), a lista é passada ao **`fzf --filter`**, que dá a ordem. O plugin procura o `fzf` no `PATH` e nos lugares comuns (linuxbrew, `~/.fzf/bin`, `~/.local/bin`). Sem fzf, ou com um fzf antigo demais (sem `--scheme`), usa a ordem interna, que dá mais peso ao nome do arquivo e a caminhos mais curtos.

## Teclas e mouse (com a caixa aberta)

| Ação | Efeito |
|---|---|
| digitar | busca (teclas mortas funcionam: `´` + `a` = `á`) |
| `↑` / `↓`, `Tab` / `Shift+Tab`, `Ctrl+P` / `Ctrl+N` | move a seleção |
| `PgUp` / `PgDn` | move 5 de uma vez |
| `Enter` | executa o selecionado |
| `Ctrl+Enter` | arquivo: abre a pasta onde ele está |
| `Backspace` / `Ctrl+Backspace`, `Ctrl+W` | apaga um caractere / uma palavra |
| `Ctrl+U` | limpa a busca |
| `Esc` | limpa a busca; com ela vazia, fecha |
| `Super+Espaço` | fecha |
| outro atalho com `Super` | fecha e o atalho acontece |
| passar o mouse / clicar num resultado | seleciona / executa |
| clicar fora da caixa | fecha |

Segurar uma tecla repete, com o atraso e a velocidade do `input:repeat_delay` e `input:repeat_rate`.

## Instalação

Mesmas dependências do hyprtab, mais `libxkbcommon` (já vem com o Hyprland). Opcionais: `fzf`, `wl-clipboard` (copiar contas), `xdg-utils` (abrir arquivos). Na pasta `find/`:

```sh
make
```

Gera `find/hyprfind.so`.

## Config (`hyprland.conf`, até 0.56)

```ini
plugin = /caminho/para/hyprtab/find/hyprfind.so
bind = SUPER, SPACE, hyprfind:toggle

plugin {
    hyprfind {
        width = 680     # largura da caixa, px
        files = 1       # 0: sem a seção de arquivos (e sem varrer o ~)
        fzf = auto      # auto, off ou o caminho do executável
    }
}
```

`hyprfind:toggle` aceita um texto para já abrir buscando: `hyprctl dispatch hyprfind:toggle firefox`.

## Config Lua (`hyprland.lua`, 0.57+)

```lua
hl.plugin.load("/caminho/para/hyprtab/find/hyprfind.so")
hl.bind("SUPER + SPACE", function() hl.plugin.hyprfind.toggle() end)
```

## Diagnóstico

`hyprctl hyprfind` mostra o estado: se está aberta, a busca, os resultados, quantos apps e arquivos estão indexados e qual fzf está em uso.

## Testes

```sh
make test              # modelo: busca fuzzy, calculadora, .desktop, varredura, fzf, lista
tests/nested/run.sh    # num Hyprland aninhado: digitação (wtype), seções, Enter, Esc, cliques, unload/load
```
