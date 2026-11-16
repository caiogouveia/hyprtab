#!/usr/bin/env bash
# Teste do hyprborders num Hyprland ANINHADO (janela dentro da sessão atual), sob gdb.
# Nunca toca a sessão real: todo hyprctl vai para a instância que este script criou; se ela não for
# identificada, o script aborta.
#
#   ./run.sh                 checagens + 60 ciclos de estresse
#   ./run.sh -n 20           20 ciclos
#   ./run.sh -s 600          ... e depois 600 s de uso contínuo (mede a memória)
#   ./run.sh -k              deixa o aninhado aberto no fim (para olhar)
#   ./run.sh -m              só o hyprborders (sem hyprbars, borders-plus-plus e os outros plugins do repo)
#
# Sai com 0 se tudo passou; com 1 e o backtrace do gdb se o Hyprland caiu ou travou.
#
# Por que LIBSEAT_BACKEND=seatd: sem isso o aninhado abre uma seat do logind que fica inativa e não
# desenha nada (tela preta, "Attempted to render frame on inactive session!"). Sem seatd rodando, a
# libseat falha, não há sessão e só o backend Wayland é usado.

set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
SO="$REPO/borders/hyprborders.so"
SYSPLUG=/usr/lib/x86_64-linux-gnu/hyprland/plugins
TERM_APP=${TERM_APP:-foot}

CYCLES=60 SOAK=0 KEEP=0 MINIMAL=0
while getopts "n:s:km" o; do
    case $o in
        n) CYCLES=$OPTARG ;;
        s) SOAK=$OPTARG ;;
        k) KEEP=1 ;;
        m) MINIMAL=1 ;;
        *) sed -n '2,15p' "$0"; exit 2 ;;
    esac
done

die() { echo "ERRO: $*" >&2; exit 2; }
[ -f "$SO" ] || die "$SO não existe (rode make antes)"
command -v Hyprland >/dev/null || die "Hyprland não encontrado"
command -v "$TERM_APP" >/dev/null || die "terminal '$TERM_APP' não encontrado (use TERM_APP=...)"
[ -n "${WAYLAND_DISPLAY:-}" ] || die "rode de dentro de uma sessão Wayland"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/hyprborders-nested.XXXXXX")
cp "$HERE/nested.conf" "$WORK/hyprland.conf"
GDBLOG="$WORK/gdb.log"
LAUNCHER_PID="" HPID="" SIG=""

cleanup() {
    if [ "$KEEP" = 1 ] && [ -n "$HPID" ] && kill -0 "$HPID" 2>/dev/null; then
        echo "aninhado mantido aberto: HYPRLAND_INSTANCE_SIGNATURE=$SIG  (encerrar: kill $HPID)"
        return
    fi
    [ -n "$HPID" ] && kill "$HPID" 2>/dev/null
    [ -n "$LAUNCHER_PID" ] && kill "$LAUNCHER_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

# ---------- sobe o aninhado ----------

instances() { hyprctl instances -j 2>/dev/null | python3 -c 'import json,sys; [print(i["instance"], i["pid"]) for i in json.load(sys.stdin)]'; }
BEFORE=$(instances)

LAUNCH=(Hyprland -c "$WORK/hyprland.conf")
if command -v gdb >/dev/null; then
    LAUNCH=(gdb -q -batch -ex "handle SIGPIPE nostop noprint" -ex "handle SIGUSR1 nostop noprint"
            -ex "handle SIGUSR2 nostop noprint" -ex run -ex "bt 30" --args "${LAUNCH[@]}")
else
    echo "aviso: gdb não encontrado, sem backtrace em caso de queda"
fi
(cd "$WORK" && exec env -u HYPRLAND_INSTANCE_SIGNATURE LIBSEAT_BACKEND=seatd HYPRLAND_NO_CRASHREPORTER=1 \
    "${LAUNCH[@]}") >"$GDBLOG" 2>&1 &
LAUNCHER_PID=$!

# a instância nova é a que não existia antes (nunca "a mais recente": poderia ser a sessão real)
for _ in $(seq 1 60); do
    NEW=$(comm -13 <(echo "$BEFORE" | sort) <(instances | sort) | head -1)
    [ -n "$NEW" ] && break
    sleep 0.5
done
[ -n "${NEW:-}" ] || { tail -20 "$GDBLOG"; die "o aninhado não subiu"; }
read -r SIG HPID <<<"$NEW"
export HYPRLAND_INSTANCE_SIGNATURE=$SIG
for _ in $(seq 1 40); do timeout 2 hyprctl version >/dev/null 2>&1 && break; sleep 0.5; done
echo "aninhado: pid $HPID, log em $WORK"

h() { hyprctl "$@" >/dev/null 2>&1; }
nwin() { hyprctl clients 2>/dev/null | grep -c '^Window'; }
# fecha as janelas mais antigas até sobrarem $1 (por endereço: killactive não fecha nada se o foco estiver
# num workspace vazio, e o laço não terminaria)
trim() {
    local a
    for a in $(hyprctl clients -j 2>/dev/null | python3 -c "import json,sys
c = sorted(json.load(sys.stdin), key=lambda c: -c['focusHistoryID'])
print(' '.join(x['address'] for x in c[:max(0, len(c) - $1)]))"); do
        h dispatch closewindow "address:$a"
    done
}
crashed() {
    echo "FALHOU: $1"
    grep -A40 -E "SIGSEGV|SIGABRT|SIGBUS|SIGFPE" "$GDBLOG" | grep '^#' | head -16
    exit 1
}
alive() {
    kill -0 "$HPID" 2>/dev/null || crashed "$1 (Hyprland caiu)"
    timeout 3 hyprctl version >/dev/null 2>&1 || crashed "$1 (Hyprland travou)"
}
check() { # check "descrição" condição...
    local d=$1; shift
    if "$@"; then echo "  ok    $d"; else echo "  FALHA $d"; FAILS=$((FAILS + 1)); fi
}
FAILS=0
loaded() { hyprctl plugin list 2>/dev/null | grep -q '^Plugin hyprborders '; }

h output create wayland
sleep 1

PLUGINS=()
if [ "$MINIMAL" = 0 ]; then
    for p in "$SYSPLUG/libhyprbars.so" "$SYSPLUG/libborders-plus-plus.so" "$REPO/hyprtab.so" \
             "$REPO/dock/hyprdock.so" "$REPO/menubar/hyprmenu.so" "$REPO/pager/hyprpager.so"; do
        [ -f "$p" ] && PLUGINS+=("$p")
    done
fi
for p in "${PLUGINS[@]}"; do
    out=$(hyprctl plugin load "$p" 2>&1)
    [ "$out" = ok ] || echo "  aviso: $(basename "$p"): $out"
done
h plugin load "$SO"
alive "carregar plugins"

# ---------- checagens ----------

echo "checagens:"
check "hyprborders carregado (+ ${#PLUGINS[@]} outros plugins)" loaded
check "2 monitores" test "$(hyprctl monitors 2>/dev/null | grep -c '^Monitor')" -eq 2

h dispatch focusmonitor WAYLAND-1
h dispatch exec "$TERM_APP"
h dispatch exec "$TERM_APP"
h dispatch focusmonitor WAYLAND-2
h dispatch exec "$TERM_APP"
for _ in $(seq 1 20); do [ "$(nwin)" -ge 3 ] && break; sleep 0.25; done
check "janelas abertas nos 2 monitores" test "$(nwin)" -ge 3
alive "abrir janelas"

h plugin unload "$SO"; sleep 0.3
check "descarregar com janelas abertas" sh -c "! hyprctl plugin list | grep -q '^Plugin hyprborders '"
h plugin load "$SO"; sleep 0.3
check "carregar de novo" loaded
alive "unload/load"

h dispatch focusmonitor WAYLAND-1
h dispatch fullscreen 0; sleep 0.4
h plugin unload "$SO"; h plugin load "$SO"; sleep 0.3
alive "unload/load em tela cheia"
h dispatch fullscreen 0
h dispatch fullscreen 1; sleep 0.4
h dispatch fullscreen 1
check "tela cheia e maximizar" loaded

sed -i 's/^\(\s*shadow_blur\s*=\s*\).*/\140/' "$WORK/hyprland.conf"
h reload; sleep 1
check "reload aplica shadow_blur = 40" sh -c "hyprctl getoption plugin:hyprborders:shadow_blur | grep -q 'int: 40'"
for _ in 1 2 3 4 5; do h reload; sleep 0.2; done
check "5 reloads seguidos, plugin continua" loaded
alive "reload"

# ---------- estresse ----------

echo "estresse: $CYCLES ciclos de unload/load no meio de tudo"
for i in $(seq 1 "$CYCLES"); do
    h dispatch focusmonitor WAYLAND-$(((i % 2) + 1))
    h plugin unload "$SO"
    h dispatch exec "[size $((200 + i % 200)) 160] $TERM_APP"
    sleep 0.25
    h plugin load "$SO"
    h dispatch movewindow mon:+1
    case $((i % 6)) in
        0) h dispatch fullscreen 0; sleep 0.2; h plugin unload "$SO"; h plugin load "$SO"; h dispatch fullscreen 0 ;;
        1) h dispatch fullscreen 1; sleep 0.2; h dispatch fullscreen 1 ;;
        2) h dispatch movetoworkspacesilent special:magic; h dispatch togglespecialworkspace magic; sleep 0.2
           h plugin unload "$SO"; h plugin load "$SO"; h dispatch togglespecialworkspace magic ;;
        3) h reload ;;
        4) h dispatch killactive; h plugin unload "$SO"; sleep 0.05; h plugin load "$SO" ;; # janela em fade-out
        5) h dispatch workspace $(((i % 4) + 1)) ;;                                         # janelas ocultas
    esac
    [ $((i % 3)) = 0 ] && h dispatch killactive
    trim 14
    sleep 0.2
    alive "ciclo $i"
    [ $((i % 10)) = 0 ] && echo "  $i/$CYCLES"
done
check "$CYCLES ciclos sem queda" true
check "plugin carregado no fim" loaded
check "render_power do usuário intacto (1)" sh -c "hyprctl getoption decoration:shadow:render_power | grep -q 'int: 1'"

# ---------- uso contínuo ----------

if [ "$SOAK" -gt 0 ]; then
    rss() { awk '/VmRSS/{print $2}' "/proc/$HPID/status"; }
    echo "uso contínuo: ${SOAK}s"
    R0=$(rss) end=$((SECONDS + SOAK)) n=0
    while [ $SECONDS -lt $end ]; do
        n=$((n + 1))
        case $((RANDOM % 8)) in
            0) [ "$(nwin)" -lt 12 ] && h dispatch exec "[size $((150 + RANDOM % 300)) $((120 + RANDOM % 200))] $TERM_APP" ;;
            1) [ "$(nwin)" -gt 4 ] && h dispatch killactive ;;
            2) h dispatch moveactive $((RANDOM % 41 - 20)) $((RANDOM % 41 - 20)) ;;
            3) h dispatch resizeactive $((RANDOM % 31 - 15)) $((RANDOM % 31 - 15)) ;;
            4) h dispatch cyclenext ;;
            5) h dispatch workspace $((RANDOM % 3 + 1)) ;;
            6) h dispatch focusmonitor WAYLAND-$((RANDOM % 2 + 1)) ;;
            7) h dispatch fullscreen $((RANDOM % 2)); sleep 0.3; h dispatch fullscreen 0 ;;
        esac
        sleep 0.15
        [ $((n % 400)) = 0 ] && echo "  $((SOAK - (end - SECONDS)))s, $n ações, RSS $(rss) kB"
        alive "uso contínuo (ação $n)"
    done
    check "uso contínuo: $n ações sem queda (RSS $R0 -> $(rss) kB)" true
fi

echo
if [ "$FAILS" = 0 ]; then echo "PASSOU"; else echo "$FAILS checagem(ns) falharam"; exit 1; fi
