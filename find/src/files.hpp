#pragma once
// Busca de arquivos do hyprfind, fora da thread do compositor. Sem dependência do Hyprland.
//
// A lista de arquivos vem de uma varredura própria do ~ (sem ocultos, sem node_modules e afins),
// guardada em memória e refeita quando fica velha. Cada busca passa a lista pelo `fzf --filter`;
// sem fzf (ou se ele falhar), usa hf::rankFiles.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hf {

// arquivos e pastas sob `root`, relativos a ele (pastas terminam em '/'), sem seguir links.
// Para em `limit` entradas ou quando `stop` virar true.
std::vector<std::string> walkFiles(const std::string& root, size_t limit, const std::atomic<bool>& stop, int maxDepth = 12);

// executável do fzf: PATH e os lugares comuns (linuxbrew, ~/.fzf/bin, ~/.local/bin); "" se não houver
std::string findFzf(const std::string& home);

// `fzf --filter query` sobre `blob` (uma entrada por linha); as `limit` primeiras linhas da saída.
// nullopt se o fzf não rodou ou saiu com erro. `cancelled` é consultado enquanto espera.
std::optional<std::vector<std::string>> fzfFilter(const std::string& fzf, const std::string& query, const std::string& blob, size_t limit,
                                                  const std::function<bool()>& cancelled);

class FileSearch {
  public:
    // `fzf` vazio: só a ordem interna
    void start(const std::string& home, const std::string& fzf, size_t limit = 8);
    void stop();
    // refaz a lista se estiver velha (chamado ao abrir)
    void warm();
    // nova busca; uma busca mais nova cancela a anterior
    void request(uint64_t gen, const std::string& query);
    // resultado novo desde o último take? (gen da busca que o produziu)
    bool take(uint64_t& gen, std::vector<std::string>& out);
    bool busy();

    size_t      indexed() const { return m_count; }
    std::string fzf() const { return m_fzf; }

  private:
    void run();
    void buildIndex();

    std::string             m_home, m_fzf;
    size_t                  m_limit = 8;
    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool>       m_stop{false};
    std::atomic<uint64_t>   m_reqGen{0};
    std::atomic<size_t>     m_count{0};

    // protegidos por m_mutex
    bool                     m_hasJob = false, m_warm = false, m_fresh = false, m_working = false;
    uint64_t                 m_jobGen = 0, m_doneGen = 0;
    std::string              m_job;
    std::vector<std::string> m_out;

    // só da thread de trabalho
    std::vector<std::string>              m_index;
    std::string                           m_blob;
    std::chrono::steady_clock::time_point m_builtAt{};
};

} // namespace hf
