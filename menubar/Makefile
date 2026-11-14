# O hyprmenu é um único .so. Usa o código comum do hyprtab em ../src (render, ícones, compat).
# D-Bus pelo sd-bus da libsystemd (C puro). Para SVG nos ícones, instale librsvg (detectado sozinho).
RSVG := $(shell pkg-config --exists librsvg-2.0 && echo 1)

CXXFLAGS_EXTRA := $(if $(RSVG),-DHYPRTAB_RSVG,)
PKGS := pixman-1 libdrm hyprland pangocairo cairo libsystemd $(if $(RSVG),librsvg-2.0,)
SHARED := ../src/render.cpp ../src/icons.cpp ../src/clients.cpp

all:
	$(CXX) -shared -fPIC --no-gnu-unique src/*.cpp $(SHARED) -o hyprmenu.so -g -std=c++2b $(CXXFLAGS_EXTRA) \
		`pkg-config --cflags $(PKGS)` `pkg-config --libs libsystemd`

# testes do modelo (árvore, geometria, hit-test), sem dependência do Hyprland
test:
	$(CXX) -std=c++2b -g -Wall -Wextra -fsanitize=address,undefined -D_GLIBCXX_ASSERTIONS tests/test.cpp src/model.cpp -o tests/run
	./tests/run

clean:
	rm -f ./hyprmenu.so ./tests/run

.PHONY: all test clean
