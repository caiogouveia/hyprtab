# Compila e testa todos os plugins. Cada um tem o seu Makefile em plugins/<nome>/.
# Código compartilhado (compat, render, ícones, parser do hyprctl) fica em common/.
# borders (hyprborders) ainda não compila no 0.57 (header removido): fica de fora; `make -C plugins/borders` à parte
PLUGINS := hyprtab dock menubar pager find

all test clean:
	@for p in $(PLUGINS); do $(MAKE) -C plugins/$$p $@ || exit 1; done

.PHONY: all test clean
