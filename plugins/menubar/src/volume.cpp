#include "volume.hpp"

#include <wayland-server-core.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <chrono>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <format>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace volume {
namespace {

using Clock = std::chrono::steady_clock;
using Argv  = std::vector<std::string>;

constexpr auto        KILL_AFTER = std::chrono::seconds(3); // comando travado (PipeWire fora do ar)
const std::string     SINK       = "@DEFAULT_AUDIO_SINK@";

enum Kind { GET, STATUS, WRITE, KINDS };

struct Job {
    Kind              kind;
    pid_t             pid = -1;
    int               fd  = -1;
    wl_event_source*  src = nullptr;
    std::string       out;
    Clock::time_point started;
    uint64_t          gen = 0; // g_writeGen quando começou (leituras antigas são descartadas)
    bool              running() const {
        return pid > 0;
    }
};

Job              g_jobs[KINDS] = {{GET}, {STATUS}, {WRITE}};
wl_event_loop*   g_loop        = nullptr;
void             (*g_onChange)() = nullptr;
State            g_state;
std::string      g_err;
std::deque<Argv> g_writes;
uint64_t         g_writeGen = 0; // muda a cada mudança local
bool             g_wantSinks = false;

void changed() {
    if (g_onChange)
        g_onChange();
}

void finish(Job& j);

int onFd(int fd, uint32_t mask, void* data) {
    auto& j = *(Job*)data;
    char  buf[4096];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            if (j.out.size() < (1 << 20))
                j.out.append(buf, n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)))
            return 0;
        break; // fim (0), erro, ou hangup sem mais nada
    }
    finish(j);
    return 0;
}

bool run(Job& j, const Argv& argv) {
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0)
        return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t def, none;
    sigemptyset(&def);
    sigaddset(&def, SIGCHLD);
    sigaddset(&def, SIGPIPE);
    sigemptyset(&none);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

    std::vector<char*> args;
    for (auto& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t     pid = -1;
    const int rc  = posix_spawnp(&pid, args[0], &fa, &at, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(p[1]);
    if (rc != 0) {
        close(p[0]);
        g_err = std::format("{}: {}", argv[0], strerror(rc));
        return false;
    }
    j.pid     = pid;
    j.fd      = p[0];
    j.out.clear();
    j.started = Clock::now();
    j.gen     = g_writeGen;
    j.src     = wl_event_loop_add_fd(g_loop, j.fd, WL_EVENT_READABLE, onFd, &j);
    return true;
}

// devolve true se o processo terminou bem (ou não dá para saber: SIGCHLD ignorado pelo compositor)
bool reap(Job& j) {
    if (j.src)
        wl_event_source_remove(j.src);
    if (j.fd >= 0)
        close(j.fd);
    int  st = 0;
    bool ok = true;
    // a saída fechou: o processo está saindo, a espera é curta
    if (waitpid(j.pid, &st, 0) == j.pid)
        ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    j.pid = -1;
    j.fd  = -1;
    j.src = nullptr;
    return ok;
}

void pumpWrites();

void finish(Job& j) {
    const bool ok    = reap(j);
    const bool fresh = j.gen == g_writeGen && !g_jobs[WRITE].running() && g_writes.empty();
    switch (j.kind) {
        case GET: {
            if (!fresh)
                break; // uma mudança local veio depois: o valor lido já é velho
            mb::Volume v;
            if (mb::parseVolume(j.out, v)) {
                const bool diff = !g_state.ok || std::abs(v.vol - g_state.vol) > 1e-4 || v.muted != g_state.muted;
                g_state.ok      = true;
                g_state.vol     = v.vol;
                g_state.muted   = v.muted;
                g_err.clear();
                if (diff)
                    changed();
            } else {
                g_err = j.out.empty() ? "wpctl get-volume: sem saída" : "wpctl get-volume: " + j.out.substr(0, j.out.find('\n'));
                if (g_state.ok) {
                    g_state.ok = false;
                    changed();
                }
            }
            break;
        }
        case STATUS: {
            auto sinks = mb::parseSinks(j.out);
            if (!fresh)
                break;
            const bool diff = sinks.size() != g_state.sinks.size() ||
                !std::equal(sinks.begin(), sinks.end(), g_state.sinks.begin(),
                            [](const mb::Sink& a, const mb::Sink& b) { return a.id == b.id && a.def == b.def && a.name == b.name; });
            g_state.sinks = std::move(sinks);
            if (diff)
                changed();
            break;
        }
        case WRITE:
            if (!ok)
                g_err = "wpctl falhou: " + j.out.substr(0, j.out.find('\n'));
            pumpWrites();
            if (!g_jobs[WRITE].running()) // acabaram as mudanças: confere o que ficou valendo
                refresh(g_wantSinks);
            break;
        default: break;
    }
}

void pumpWrites() {
    if (g_jobs[WRITE].running() || g_writes.empty() || !g_loop)
        return;
    const auto argv = g_writes.front();
    g_writes.pop_front();
    if (!run(g_jobs[WRITE], argv))
        pumpWrites();
}

void queue(Argv argv) {
    g_writeGen++;
    // arrastar o slider: só o valor mais recente importa
    if (argv[1] == "set-volume" && !g_writes.empty() && g_writes.back()[1] == "set-volume")
        g_writes.back() = std::move(argv);
    else
        g_writes.push_back(std::move(argv));
    pumpWrites();
}

void killStuck() {
    for (auto& j : g_jobs)
        if (j.running() && Clock::now() - j.started > KILL_AFTER)
            kill(j.pid, SIGKILL); // a saída fecha e finish() roda pelo event loop
}

} // namespace

bool start(wl_event_loop* loop, void (*onChange)()) {
    g_loop     = loop;
    g_onChange = onChange;
    refresh(false);
    return true;
}

void stop() {
    for (auto& j : g_jobs)
        if (j.running()) {
            kill(j.pid, SIGKILL);
            reap(j);
        }
    g_writes.clear();
    g_loop     = nullptr;
    g_onChange = nullptr;
    g_state    = {};
}

const State& state() {
    return g_state;
}

void refresh(bool sinks) {
    if (!g_loop)
        return;
    killStuck();
    g_wantSinks = g_wantSinks || sinks;
    if (g_jobs[WRITE].running() || !g_writes.empty())
        return; // depois das mudanças
    if (!g_jobs[GET].running() && !run(g_jobs[GET], {"wpctl", "get-volume", SINK}) && g_state.ok) {
        g_state.ok = false;
        changed();
    }
    if (sinks && !g_jobs[STATUS].running())
        run(g_jobs[STATUS], {"wpctl", "status"});
    if (sinks)
        g_wantSinks = false;
}

void setVolume(double v) {
    v = std::clamp(v, 0.0, 1.0);
    if (!g_state.ok || (std::abs(v - g_state.vol) < 1e-4 && !g_state.muted))
        return;
    g_state.vol = v;
    queue({"wpctl", "set-volume", SINK, std::format("{:.3f}", v)});
    if (g_state.muted) { // mexer no volume tira o mudo (como no macOS)
        g_state.muted = false;
        queue({"wpctl", "set-mute", SINK, "0"});
    }
    changed();
}

void toggleMute() {
    if (!g_state.ok)
        return;
    g_state.muted = !g_state.muted;
    queue({"wpctl", "set-mute", SINK, g_state.muted ? "1" : "0"});
    changed();
}

void setDefault(int sinkId) {
    for (auto& s : g_state.sinks)
        s.def = s.id == sinkId;
    g_wantSinks = true;
    queue({"wpctl", "set-default", std::to_string(sinkId)});
    changed();
}

std::string status() {
    if (!g_state.ok)
        return std::format("volume: indisponível{}", g_err.empty() ? "" : " (" + g_err + ")");
    return std::format("volume: {:.0f}%{}, {} saídas{}", g_state.vol * 100, g_state.muted ? " (mudo)" : "", g_state.sinks.size(),
                       g_err.empty() ? "" : ", último erro: " + g_err);
}

} // namespace volume
