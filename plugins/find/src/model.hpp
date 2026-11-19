#pragma once
// Modelo do hyprfind, sem dependência do Hyprland: texto, busca fuzzy, calculadora, .desktop,
// montagem e ordem dos resultados, geometria da lista. Testado em tests/test.cpp.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hf {

// ---------- texto ----------

// minúsculas e sem acento (latinas comuns em UTF-8: "Configurações" -> "configuracoes")
std::string fold(std::string_view s);
// apaga o último caractere UTF-8 / a última palavra (e os espaços depois dela)
void popChar(std::string& s);
void popWord(std::string& s);
// 'texto' seguro para /bin/sh
std::string shellQuote(std::string_view s);
// "a b  c" -> {"a","b","c"}
std::vector<std::string> words(std::string_view s);

// ---------- busca fuzzy ----------

// Pontuação de `q` (uma palavra, já dobrada) em `text` (já dobrado); -1 se não casa.
// Trecho contínuo vale mais que letras espalhadas; começo do texto e começo de palavra ganham bônus.
int fuzzy(std::string_view q, std::string_view text);

// ---------- calculadora ----------

// resultado da conta, ou nullopt se não for conta (um número sozinho também não é).
// + - * / ^ ( ), x e × multiplicam, ÷ divide, % no fim de um número é "por cento",
// vírgula ou ponto decimal, funções sqrt/raiz sin/sen cos tan log ln abs e as constantes pi e e.
std::optional<double> calc(std::string_view expr);
// até 10 dígitos significativos, sem zeros sobrando; `comma`: vírgula decimal
std::string formatNumber(double v, bool comma);
// a conta foi escrita com vírgula decimal?
bool usesComma(std::string_view expr);

// ---------- apps (.desktop) ----------

struct App {
    std::string id;       // nome do arquivo sem .desktop, '/' de subpasta vira '-'
    std::string name;     // no idioma do usuário, se houver
    std::string extra;    // GenericName, Keywords, Name em inglês: casam, mas valem menos
    std::string exec;     // já sem os códigos %f %U...
    std::string icon;
    std::string tryExec;
    bool        terminal = false;
};

// "pt_BR.UTF-8" -> {"pt_BR", "pt"}
std::vector<std::string> localeLangs(std::string_view lang);
// tira %f %F %u %U %i %c %k etc. do Exec (%% vira %)
std::string stripFieldCodes(std::string_view exec);
// nullopt se não é um app para mostrar (Type, NoDisplay, Hidden, OnlyShowIn/NotShowIn)
std::optional<App> parseDesktop(const std::string& id, const std::string& content, const std::vector<std::string>& langs, std::string_view desktop);
// varre as pastas (com subpastas); a primeira que tiver um id ganha (usuário antes do sistema)
std::vector<App> loadApps(const std::vector<std::string>& dirs, const std::vector<std::string>& langs, std::string_view desktop);

// ---------- resultados ----------

enum class Kind { Calc, App, Window, File };
const char* sectionName(Kind k);

struct Window {
    std::string addr, cls, title, appName;
};

struct Result {
    Kind        kind = Kind::App;
    std::string title, subtitle;
    std::string key;  // app: id; janela: endereço; arquivo: caminho completo; conta: o valor
    std::string icon; // nome passado ao carregador de ícones
    int         score = 0;
};

struct Limits {
    size_t apps = 6, windows = 4, files = 8;
};

// Monta a lista na ordem das seções: Calculadora, Aplicativos, Janelas, Arquivos.
// `files` já vem ordenado (fzf ou rankFiles), relativo a `home`; pastas terminam em '/'.
// `usage`: id do app -> vezes que foi aberto pelo hyprfind (desempata e sobe os mais usados).
std::vector<Result> search(std::string_view query, const std::vector<App>& apps, const std::vector<Window>& windows,
                           const std::vector<std::string>& files, const std::string& home,
                           const std::unordered_map<std::string, int>& usage, const Limits& lim = {});

// ordem interna para os arquivos quando não há fzf: casa todas as palavras, nome do arquivo vale mais
std::vector<std::string> rankFiles(std::string_view query, const std::vector<std::string>& list, size_t limit);
// resultado de um arquivo (caminho relativo a home)
Result fileResult(const std::string& rel, const std::string& home);

// ---------- lista ----------

struct Row {
    int         result = -1; // índice em results; -1: título de seção
    std::string header;
    double      y = 0, h = 0; // relativo ao topo da lista
};

std::vector<Row> layoutRows(const std::vector<Result>& results, double rowH, double headH);
double           rowsHeight(const std::vector<Row>& rows);
// rolagem que deixa a linha do resultado `sel` (e o título da seção dela, se couber) na janela de
// altura `viewH`, mexendo o mínimo a partir de `cur`
double scrollFor(const std::vector<Row>& rows, int sel, double viewH, double cur);
// resultado sob o ponto `y` (relativo ao topo da área visível), ou -1
int hitRow(const std::vector<Row>& rows, double y, double scroll, double viewH);

// ---------- uso ----------

std::unordered_map<std::string, int> parseUsage(const std::string& content);
std::string                          formatUsage(const std::unordered_map<std::string, int>& usage);

} // namespace hf
