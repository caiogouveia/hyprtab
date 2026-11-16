# sgi-red

Tema de cursores no estilo do IRIX/4Dwm da SGI: pixel art vermelha com contorno
branco. Sai nos dois formatos, **XCursor** e **hyprcursor**, nos tamanhos 24, 48,
72 e 96 (o mesmo bitmap ampliado por fator inteiro, sem borrar). A ampulheta
(`wait`) é animada.

![prévia](build/preview.png) (gerada pelo `make`)

## Instalar

```sh
make install            # -> ~/.local/share/icons/sgi-red
```

No `hyprland.conf`:

```ini
env = HYPRCURSOR_THEME,sgi-red
env = HYPRCURSOR_SIZE,24
env = XCURSOR_THEME,sgi-red
env = XCURSOR_SIZE,24
```

Para trocar sem reiniciar: `hyprctl setcursor sgi-red 24`. Apps GTK leem o tema de
`gsettings set org.gnome.desktop.interface cursor-theme sgi-red`.

## Variações de cor

```sh
make install NAME=sgi-blue FILL='#2050ff'
make install NAME=sgi-red-dark OUTLINE='#000000'
```

## Editar os desenhos

Tudo está em `gen.py`: cada cursor é uma grade de caracteres no tamanho 24
(`X` = cor principal, `o` = branco interno, `.` = transparente), desenhada à mão
(`ARROW`, `HAND`, `IBEAM`, …) ou por função (setas, cantos, lupa). O contorno
branco externo é gerado sozinho. O catálogo (`catalog()`) liga cada desenho aos
nomes que os apps pedem (CSS: `default`, `pointer`, `text`, `wait`, `nwse-resize`…;
X11: `left_ptr`, `hand2`, `xterm`, `watch`…; e os hashes usados pelo Qt).
