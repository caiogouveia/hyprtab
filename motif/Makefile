# hyprmotif: molduras estilo Motif/mwm para o Hyprland (um único .so).
# Alvo atual: Hyprland 0.53 (API de decorações do hyprbars v0.53.0).
PKGS := pixman-1 libdrm hyprland pangocairo cairo

all:
	$(CXX) -shared -fPIC --no-gnu-unique src/*.cpp -o hyprmotif.so -g -std=c++2b \
		`pkg-config --cflags $(PKGS)`

# testes da geometria (sem dependência do Hyprland)
test:
	$(CXX) -std=c++2b -g -Wall -Wextra -fsanitize=address,undefined -D_GLIBCXX_ASSERTIONS tests/test.cpp src/geometry.cpp -o tests/run
	./tests/run

clean:
	rm -f ./hyprmotif.so ./tests/run

.PHONY: all test clean
