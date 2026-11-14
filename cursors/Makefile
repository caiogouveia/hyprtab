# Tema de cursores sgi-red (XCursor + hyprcursor). Só precisa de python3.
NAME    ?= sgi-red
FILL    ?= \#ff0000
OUTLINE ?= \#ffffff
PREFIX  ?= $(HOME)/.local/share/icons

all:
	python3 gen.py build/$(NAME) --name $(NAME) --fill '$(FILL)' --outline '$(OUTLINE)' --preview build/preview.png

install: all
	rm -rf $(PREFIX)/$(NAME)
	mkdir -p $(PREFIX)
	cp -a build/$(NAME) $(PREFIX)/$(NAME)

uninstall:
	rm -rf $(PREFIX)/$(NAME)

clean:
	rm -rf build

.PHONY: all install uninstall clean
