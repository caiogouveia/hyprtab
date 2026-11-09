#pragma once

#include <string>
#include <unordered_map>
#include <utility>

// endereço ("0x...") -> {classe, título}
using Clients = std::unordered_map<std::string, std::pair<std::string, std::string>>;

// Lê a saída de `hyprctl clients -j`. Entrada malformada devolve o que deu pra ler, sem lançar.
Clients parseClients(const std::string& js);
