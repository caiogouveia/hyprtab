#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// endereço ("0x...") -> {classe, título}
using Clients = std::unordered_map<std::string, std::pair<std::string, std::string>>;

// Lê a saída de `hyprctl clients -j`. Entrada malformada devolve o que deu pra ler, sem lançar.
Clients parseClients(const std::string& js);

// "a, b ,,c" -> {"a", "b", "c"} (listas da config)
std::vector<std::string> splitList(const std::string& csv);

// Janelas auxiliares que não são "apps" (seletor de compartilhar tela, portais, polkit, diálogos do
// Hyprland): fora do switcher e da dock por padrão, junto com o `ignore` da config de cada um.
inline const std::vector<std::string> DEFAULT_IGNORE = {
    "hyprland-share-picker", "xdg-desktop-portal*", "hyprpolkitagent", "polkit-*", "*polkit-kde-authentication-agent*", "hyprland-dialog",
    "hyprland-update-screen", "hyprland-welcome",
};

// a classe casa com algum padrão? Sem diferenciar maiúsculas; `*` no começo e/ou no fim é curinga.
bool classIgnored(const std::string& cls, const std::vector<std::string>& patterns);
