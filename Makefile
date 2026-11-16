# Compila e testa todos os plugins. Cada um tem o seu Makefile em plugins/<nome>/.
# Código compartilhado (compat, render, ícones, parser do hyprctl) fica em common/.
PLUGINS := hyprtab dock menubar pager find borders

all test clean:
	@for p in $(PLUGINS); do $(MAKE) -C plugins/$$p $@ || exit 1; done

.PHONY: all test clean
