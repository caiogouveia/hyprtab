# O hyprtab é um único .so. Para SVG nos ícones, instale librsvg (detectado sozinho).
RSVG := $(shell pkg-config --exists librsvg-2.0 && echo 1)

CXXFLAGS_EXTRA := $(if $(RSVG),-DHYPRTAB_RSVG,)
PKGS := pixman-1 libdrm hyprland pangocairo cairo $(if $(RSVG),librsvg-2.0,)

all:
	$(CXX) -shared -fPIC --no-gnu-unique src/*.cpp -o hyprtab.so -g -std=c++2b $(CXXFLAGS_EXTRA) \
		`pkg-config --cflags $(PKGS)`

clean:
	rm -f ./hyprtab.so
