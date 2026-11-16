#!/usr/bin/env bash
# Teste do hyprfind num Hyprland ANINHADO (janela dentro da sessão atual). Nunca toca a sessão real:
# todo hyprctl/wtype vai para a instância que este script criou; se ela não for identificada, aborta.
#
#   ./run.sh          checagens + 20 ciclos de unload/load
#   ./run.sh -n 50    50 ciclos
#   ./run.sh -k       deixa o aninhado aberto no fim (para olhar)
#
# Precisa de wtype, wl-clipboard e grim. A captura da caixa aberta fica em $WORK/aberto.png.
# LIBSEAT_BACKEND=seatd: sem isso o aninhado fica preto (ver borders/tests/nested/run.sh).

set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
SO="$REPO/find/hyprfind.so"
TERM_APP=${TERM_APP:-foot}

CYCLES=20 KEEP=0
while getopts "n:k" o; do
    case $o in
        n) CYCLES=$OPTARG ;;
        k) KEEP=1 ;;
        *) sed -n '2,10p' "$0"; exit 2 ;;
    esac
done

die() { echo "ERRO: $*" >&2; exit 2; }
[ -f "$SO" ] || die "$SO não existe (rode make antes)"
for c in Hyprland wtype wl-paste grim "$TERM_APP"; do command -v "$c" >/dev/null || die "$c não encontrado"; done
[ -n "${WAYLAND_DISPLAY:-}" ] || die "rode de dentro de uma sessão Wayland"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/hyprfind-nested.XXXXXX")
cat >"$WORK/hyprland.conf" <<'EOF'
monitor = WAYLAND-1, 1280x800, 0x0, 1
misc {
    disable_hyprland_logo = true
    disable_splash_rendering = true
}
debug {
    disable_logs = false
}
bind = SUPER, SPACE, hyprfind:toggle
EOF
GDBLOG="$WORK/gdb.log"
LAUNCHER_PID="" HPID="" SIG=""

cleanup() {
    if [ "$KEEP" = 1 ] && [ -n "$HPID" ] && kill -0 "$HPID" 2>/dev/null; then
        echo "aninhado mantido aberto: HYPRLAND_INSTANCE_SIGNATURE=$SIG WAYLAND_DISPLAY=$NWL  (encerrar: kill $HPID)"
        return
    fi
    [ -n "$HPID" ] && kill "$HPID" 2>/dev/null
    [ -n "$LAUNCHER_PID" ] && kill "$LAUNCHER_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

instances() { hyprctl instances -j 2>/dev/null | python3 -c 'import json,sys; [print(i["instance"], i["pid"], i["wl_socket"]) for i in json.load(sys.stdin)]'; }
BEFORE=$(instances)

LAUNCH=(Hyprland -c "$WORK/hyprland.conf")
if command -v gdb >/dev/null; then
    LAUNCH=(gdb -q -batch -ex "handle SIGPIPE nostop noprint" -ex "handle SIGUSR1 nostop noprint"
            -ex "handle SIGUSR2 nostop noprint" -ex "handle SIGCHLD nostop noprint" -ex run -ex "bt 30" --args "${LAUNCH[@]}")
fi
(cd "$WORK" && exec env -u HYPRLAND_INSTANCE_SIGNATURE LIBSEAT_BACKEND=seatd HYPRLAND_NO_CRASHREPORTER=1 \
    "${LAUNCH[@]}") >"$GDBLOG" 2>&1 &
LAUNCHER_PID=$!

for _ in $(seq 1 60); do
    NEW=$(comm -13 <(echo "$BEFORE" | sort) <(instances | sort) | head -1)
    [ -n "$NEW" ] && break
    sleep 0.5
done
[ -n "${NEW:-}" ] || { tail -20 "$GDBLOG"; die "o aninhado não subiu"; }
read -r SIG HPID NWL <<<"$NEW"
export HYPRLAND_INSTANCE_SIGNATURE=$SIG
for _ in $(seq 1 40); do timeout 2 hyprctl version >/dev/null 2>&1 && break; sleep 0.5; done
echo "aninhado: pid $HPID, $NWL, log em $WORK"

h() { hyprctl "$@" >/dev/null 2>&1; }
# teclas para o aninhado (nunca para a sessão real)
wt() { WAYLAND_DISPLAY=$NWL wtype "$@"; sleep 0.15; }
st() { hyprctl hyprfind 2>/dev/null; }
crashed() {
    echo "FALHOU: $1"
    grep -A40 -E "SIGSEGV|SIGABRT|SIGBUS|SIGFPE" "$GDBLOG" | grep '^#' | head -16
    exit 1
}
alive() {
    kill -0 "$HPID" 2>/dev/null || crashed "$1 (Hyprland caiu)"
    timeout 3 hyprctl version >/dev/null 2>&1 || crashed "$1 (Hyprland travou)"
}
FAILS=0
check() {
    local d=$1; shift
    if "$@"; then echo "  ok    $d"; else echo "  FALHA $d"; FAILS=$((FAILS + 1)); fi
}
has() { st | grep -qF -- "$1"; }
first() { st | grep -m1 '^[> ] \[' ; }

out=$(hyprctl plugin load "$SO" 2>&1)
[ "$out" = ok ] || die "plugin load: $out"
alive "carregar"
h dispatch exec "$TERM_APP"
sleep 1

echo "checagens:"
h dispatch hyprfind:toggle
check "abre pelo dispatcher" has "aberto: true"

wt "foot"
check "digitar vai para a busca" has 'consulta: "foot"'
check "acha o app foot" sh -c "hyprctl hyprfind | grep -q '\[Aplicativos\] Foot'"
check "acha a janela do foot" sh -c "hyprctl hyprfind | grep -q '\[Janelas abertas\]'"

wt -k BackSpace -k BackSpace -k BackSpace -k BackSpace
check "backspace apaga" has 'consulta: ""'

wt "2+3*4"
check "conta vem primeiro" sh -c "hyprctl hyprfind | grep -m1 '^> ' | grep -qF '[Calculadora] = 14'"
wt -k Return; sleep 0.4
check "Enter fecha" has "aberto: false"
check "Enter copia o resultado" test "$(WAYLAND_DISPLAY=$NWL timeout 3 wl-paste -n 2>/dev/null)" = 14

h dispatch hyprfind:toggle
wt "ação"
check "acentos digitados" has 'consulta: "ação"'
wt -k Escape
check "Esc limpa a busca" sh -c "hyprctl hyprfind | grep -q 'consulta: \"\"' && hyprctl hyprfind | grep -q 'aberto: true'"

wt "hyprfind"
for _ in $(seq 1 40); do has "[Arquivos]" && break; sleep 0.25; done
check "arquivos aparecem (busca na thread)" has "[Arquivos]"
st | grep -E "^(fzf|arquivos indexados)"
wt -k Down -k Down
check "seta para baixo move a seleção" has "selecionado: 2"
WAYLAND_DISPLAY=$NWL timeout 5 grim "$WORK/aberto.png" 2>/dev/null
check "captura da caixa aberta" test -s "$WORK/aberto.png"

wt -M logo -k space -m logo
check "Super+Espaço fecha" has "aberto: false"
# (atalhos do Hyprland não disparam com teclas do wtype no aninhado: reabre pelo dispatcher)
h dispatch hyprfind:toggle
check "abre de novo" has "aberto: true"
wt -k Escape
check "Esc com a busca vazia fecha" has "aberto: false"

# clique fora fecha
h dispatch hyprfind:toggle
h dispatch movecursor 20 780
sleep 0.1
WAYLAND_DISPLAY=$NWL timeout 3 wtype -k Escape -k Escape >/dev/null 2>&1
check "fechou" has "aberto: false"
alive "checagens"

echo "estresse: $CYCLES ciclos de unload/load com a caixa aberta e busca de arquivos em andamento"
for i in $(seq 1 "$CYCLES"); do
    h dispatch hyprfind:toggle
    wt "re"
    [ $((i % 2)) = 0 ] && sleep 0.15 # às vezes no meio da busca de arquivos
    h plugin unload "$SO"
    alive "unload $i"
    h plugin load "$SO"
    alive "load $i"
done
check "$CYCLES ciclos sem queda" true
h dispatch hyprfind:toggle
wt "foot"
check "funciona depois dos ciclos" sh -c "hyprctl hyprfind | grep -q '\[Aplicativos\] Foot'"
wt -k Escape -k Escape
alive "fim"

echo
echo "captura: $WORK/aberto.png"
if [ "$FAILS" = 0 ]; then echo "PASSOU"; else echo "$FAILS checagem(ns) falharam"; exit 1; fi
