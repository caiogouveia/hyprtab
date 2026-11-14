# O hyprfind é um único .so. Usa o código comum do hyprtab em ../src (render, ícones, compat).
# Para SVG nos ícones, instale librsvg (detectado sozinho). O fzf é opcional (só na busca de arquivos).
RSVG := $(shell pkg-config --exists librsvg-2.0 && echo 1)

CXXFLAGS_EXTRA := $(if $(RSVG),-DHYPRTAB_RSVG,)
PKGS := pixman-1 libdrm hyprland pangocairo cairo xkbcommon $(if $(RSVG),librsvg-2.0,)
SHARED := ../src/render.cpp ../src/icons.cpp ../src/clients.cpp

all:
	$(CXX) -shared -fPIC --no-gnu-unique src/*.cpp $(SHARED) -o hyprfind.so -g -std=c++2b $(CXXFLAGS_EXTRA) \
		`pkg-config --cflags $(PKGS)`

# testes do modelo (busca, calculadora, .desktop, arquivos, lista), sem dependência do Hyprland
test:
	$(CXX) -std=c++2b -g -Wall -Wextra -fsanitize=address,undefined -D_GLIBCXX_ASSERTIONS tests/test.cpp src/model.cpp src/files.cpp -o tests/run
	./tests/run

clean:
	rm -f ./hyprfind.so ./tests/run

.PHONY: all test clean
