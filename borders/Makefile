# O hyprborders é um único .so. Usa o compat.hpp do hyprtab em ../src.
PKGS := pixman-1 libdrm hyprland

all:
	$(CXX) -shared -fPIC --no-gnu-unique src/*.cpp -o hyprborders.so -g -std=c++2b `pkg-config --cflags $(PKGS)`

# testes do modelo (medidas da sombra), sem dependência do Hyprland
test:
	$(CXX) -std=c++2b -g -Wall -Wextra -fsanitize=address,undefined -D_GLIBCXX_ASSERTIONS tests/test.cpp src/model.cpp -o tests/run
	./tests/run

clean:
	rm -f ./hyprborders.so ./tests/run

.PHONY: all test clean
