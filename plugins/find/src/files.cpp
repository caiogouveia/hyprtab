#include "files.hpp"
#include "model.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>

extern char** environ;

namespace fs = std::filesystem;

namespace hf {

namespace {

// pastas que só têm coisa gerada: entram muitas entradas inúteis e deixam a varredura lenta
const std::set<std::string> SKIP_DIRS = {"node_modules", "__pycache__", "venv", "site-packages", "snap", "go", "vendor", "target", "build-cache"};

// a lista fica velha depois disso: ao abrir, é refeita (em segundo plano)
constexpr auto INDEX_MAX_AGE = std::chrono::seconds(90);
constexpr size_t INDEX_LIMIT = 400000;

} // namespace

std::vector<std::string> walkFiles(const std::string& root, size_t limit, const std::atomic<bool>& stop, int maxDepth) {
    std::vector<std::string>                 out;
    std::vector<std::pair<fs::path, int>>    stack = {{fs::path(root), 0}};
    const size_t                             cut   = fs::path(root).string().size() + 1;
    while (!stack.empty() && out.size() < limit && !stop) {
        auto [dir, depth] = stack.back();
        stack.pop_back();
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        std::vector<fs::path> sub;
        for (; !ec && it != fs::directory_iterator() && out.size() < limit; it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (name.empty() || name[0] == '.')
                continue;
            const auto st = it->symlink_status(ec);
            if (ec)
                break;
            const auto rel = it->path().string().substr(cut);
            if (fs::is_directory(st)) {
                if (SKIP_DIRS.contains(name))
                    continue;
                out.push_back(rel + "/");
                if (depth + 1 < maxDepth)
                    sub.push_back(it->path());
            } else if (fs::is_regular_file(st) || fs::is_symlink(st))
                out.push_back(rel);
        }
        // em profundidade, mas na ordem alfabética de cada pasta
        std::sort(sub.rbegin(), sub.rend());
        for (auto& s : sub)
            stack.push_back({s, depth + 1});
    }
    return out;
}

std::string findFzf(const std::string& home) {
    std::vector<std::string> dirs;
    if (const char* p = getenv("PATH")) {
        std::string path = p;
        size_t      a    = 0;
        while (a <= path.size()) {
            size_t b = path.find(':', a);
            if (b == std::string::npos)
                b = path.size();
            if (b > a)
                dirs.push_back(path.substr(a, b - a));
            a = b + 1;
        }
    }
    for (auto d : {"/home/linuxbrew/.linuxbrew/bin", "/usr/local/bin", "/usr/bin"})
        dirs.push_back(d);
    for (auto d : {"/.linuxbrew/bin", "/.fzf/bin", "/.local/bin", "/.nix-profile/bin", "/bin"})
        dirs.push_back(home + d);
    for (auto& d : dirs) {
        const auto f = d + "/fzf";
        if (access(f.c_str(), X_OK) == 0)
            return f;
    }
    return "";
}

std::optional<std::vector<std::string>> fzfFilter(const std::string& fzf, const std::string& query, const std::string& blob, size_t limit,
                                                  const std::function<bool()>& cancelled) {
    // stdin por socket (send com MSG_NOSIGNAL: se o fzf sair antes, nada de SIGPIPE no compositor)
    int in[2], out[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, in) != 0)
        return std::nullopt;
    if (pipe2(out, O_CLOEXEC) != 0) {
        close(in[0]);
        close(in[1]);
        return std::nullopt;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[1], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);   // o compositor pode ter sinais bloqueados
    posix_spawnattr_setsigdefault(&attr, &all); // e tratadores próprios
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<std::string> args = {fzf, "--filter", query, "--scheme=path"};
    std::vector<char*>       argv;
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t      pid = -1;
    const int  rc  = posix_spawn(&pid, fzf.c_str(), &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(in[1]);
    close(out[1]);
    if (rc != 0) {
        close(in[0]);
        close(out[0]);
        return std::nullopt;
    }
    fcntl(in[0], F_SETFL, O_NONBLOCK);
    fcntl(out[0], F_SETFL, O_NONBLOCK);

    std::vector<std::string> lines;
    std::string              buf;
    size_t                   off    = 0;
    int                      inFd   = in[0];
    bool                     eof    = false, aborted = false;
    if (blob.empty()) {
        close(inFd);
        inFd = -1;
    }
    while (!eof && lines.size() < limit) {
        if (cancelled && cancelled()) {
            aborted = true;
            break;
        }
        pollfd fds[2];
        nfds_t n = 0;
        fds[n++] = {out[0], POLLIN, 0};
        if (inFd >= 0)
            fds[n++] = {inFd, POLLOUT, 0};
        if (poll(fds, n, 50) < 0 && errno != EINTR)
            break;
        if (inFd >= 0 && (fds[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
            const ssize_t w = send(inFd, blob.data() + off, std::min<size_t>(blob.size() - off, 1 << 16), MSG_NOSIGNAL);
            if (w > 0)
                off += (size_t)w;
            if ((w < 0 && errno != EAGAIN && errno != EINTR) || off >= blob.size()) {
                close(inFd); // fim da entrada: o fzf filtra e escreve
                inFd = -1;
            }
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char          tmp[1 << 14];
            const ssize_t r = read(out[0], tmp, sizeof tmp);
            if (r == 0)
                eof = true;
            else if (r > 0) {
                buf.append(tmp, (size_t)r);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos && lines.size() < limit) {
                    lines.push_back(buf.substr(0, nl));
                    buf.erase(0, nl + 1);
                }
            } else if (errno != EAGAIN && errno != EINTR)
                eof = true;
        }
    }
    if (inFd >= 0)
        close(inFd);
    close(out[0]);

    int status = 0;
    if (!eof)
        kill(pid, SIGKILL); // já temos o bastante (ou cancelou)
    const bool reaped = waitpid(pid, &status, 0) == pid;
    if (aborted)
        return std::nullopt;
    // 0: achou; 1: nada casou; 2 ou outro: erro (opção desconhecida, por exemplo). Se o compositor
    // já recolheu o processo (reaped falso), vale o que foi lido.
    if (reaped && eof && WIFEXITED(status) && WEXITSTATUS(status) > 1)
        return std::nullopt;
    if (reaped && eof && WIFSIGNALED(status))
        return std::nullopt;
    return lines;
}

// ---------- FileSearch ----------

void FileSearch::start(const std::string& home, const std::string& fzf, size_t limit) {
    m_home  = home;
    m_fzf   = fzf;
    m_limit = limit;
    m_stop  = false;
    m_thread = std::thread([this] { run(); });
}

void FileSearch::stop() {
    {
        std::lock_guard l(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void FileSearch::warm() {
    {
        std::lock_guard l(m_mutex);
        m_warm = true;
    }
    m_cv.notify_all();
}

void FileSearch::request(uint64_t gen, const std::string& query) {
    {
        std::lock_guard l(m_mutex);
        m_job    = query;
        m_jobGen = gen;
        m_hasJob = true;
        m_reqGen = gen;
    }
    m_cv.notify_all();
}

bool FileSearch::take(uint64_t& gen, std::vector<std::string>& out) {
    std::lock_guard l(m_mutex);
    if (!m_fresh)
        return false;
    m_fresh = false;
    gen     = m_doneGen;
    out     = std::move(m_out);
    m_out.clear();
    return true;
}

bool FileSearch::busy() {
    std::lock_guard l(m_mutex);
    return m_hasJob || m_working;
}

void FileSearch::buildIndex() {
    auto list = walkFiles(m_home, INDEX_LIMIT, m_stop);
    if (m_stop)
        return;
    std::string blob;
    for (auto& s : list) {
        blob += s;
        blob += '\n';
    }
    m_index   = std::move(list);
    m_blob    = std::move(blob);
    m_count   = m_index.size();
    m_builtAt = std::chrono::steady_clock::now();
}

void FileSearch::run() {
    // sem fzf de verdade (versão sem --scheme, por exemplo): desiste dele de vez
    if (!m_fzf.empty()) {
        auto probe = fzfFilter(m_fzf, "b", "a/b\nc\n", 4, [this] { return m_stop.load(); });
        if (!probe || probe->empty() || probe->front() != "a/b")
            m_fzf.clear();
    }
    for (;;) {
        bool        warm = false;
        uint64_t    gen  = 0;
        std::string q;
        {
            std::unique_lock l(m_mutex);
            m_working = false;
            m_cv.wait(l, [this] { return m_stop || m_hasJob || m_warm; });
            if (m_stop)
                return;
            warm      = m_warm;
            m_warm    = false;
            m_working = true;
            if (m_hasJob) {
                gen      = m_jobGen;
                q        = m_job;
                m_hasJob = false;
            }
        }
        const bool stale = m_index.empty() || std::chrono::steady_clock::now() - m_builtAt > INDEX_MAX_AGE;
        if (stale && (warm || m_index.empty()))
            buildIndex();
        if (m_stop)
            return;
        if (gen == 0 || m_reqGen != gen)
            continue; // só aquecer, ou já veio outra busca

        std::optional<std::vector<std::string>> res;
        if (!m_fzf.empty())
            res = fzfFilter(m_fzf, q, m_blob, m_limit, [this, gen] { return m_stop || m_reqGen != gen; });
        if (m_reqGen != gen || m_stop)
            continue;
        if (!res)
            res = rankFiles(q, m_index, m_limit);
        std::lock_guard l(m_mutex);
        m_doneGen = gen;
        m_out     = std::move(*res);
        m_fresh   = true;
    }
}

} // namespace hf
