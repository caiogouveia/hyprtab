#include "appmenu.hpp"

#include <systemd/sd-bus.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <ctime>
#include <format>
#include <memory>
#include <poll.h>
#include <unordered_map>
#include <vector>

namespace appmenu {

namespace {

constexpr const char* REG_NAME  = "com.canonical.AppMenu.Registrar";
constexpr const char* REG_PATH  = "/com/canonical/AppMenu/Registrar";
constexpr const char* MENU_IFACE = "com.canonical.dbusmenu";

sd_bus*          g_bus      = nullptr;
wl_event_source* g_fdSrc    = nullptr;
wl_event_source* g_timerSrc = nullptr;
void (*g_onChange)()        = nullptr;
std::string      g_error;
bool             g_owner = false; // somos o Registrar (o nome é nosso)

// slots fixos (objeto, matches, pedido do nome); liberados no stop
std::vector<sd_bus_slot*> g_slots;

struct Entry {
    std::string service, path;
};
std::unordered_map<uint32_t, Entry> g_windows; // xid -> menu registrado

// app acompanhado (o da janela em foco). As respostas assíncronas trazem o `gen` como userdata: se o
// cliente mudou nesse meio tempo, a resposta é ignorada.
struct Client {
    uint64_t     gen = 0;
    uint32_t     xid = 0;
    std::string  service, path;
    mb::Node     root;
    bool         loaded = false, fetching = false, again = false;
    sd_bus_slot* layoutMatch = nullptr;
    sd_bus_slot* propsMatch  = nullptr;
    ~Client() {
        sd_bus_slot_unref(layoutMatch);
        sd_bus_slot_unref(propsMatch);
    }
};
std::unique_ptr<Client> g_client;
uint64_t                g_gen = 0;

void changed() {
    if (g_onChange)
        g_onChange();
}

void* genPtr(uint64_t gen) {
    return (void*)(uintptr_t)gen;
}
bool current(void* ud) {
    return g_client && g_client->gen == (uint64_t)(uintptr_t)ud;
}

// ---------- event loop ----------

void teardown();

void rearm() {
    if (!g_bus)
        return;
    const int ev   = sd_bus_get_events(g_bus);
    uint32_t  mask = WL_EVENT_READABLE;
    if (ev > 0 && (ev & POLLOUT))
        mask |= WL_EVENT_WRITABLE;
    wl_event_source_fd_update(g_fdSrc, mask);

    uint64_t usec = 0;
    int      ms   = 0; // 0 desarma
    if (sd_bus_get_timeout(g_bus, &usec) >= 0 && usec != UINT64_MAX) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const uint64_t now = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
        ms                 = usec <= now ? 1 : (int)std::min<uint64_t>((usec - now + 999) / 1000, 60000);
    }
    wl_event_source_timer_update(g_timerSrc, ms);
}

void pump() {
    if (!g_bus)
        return;
    for (int i = 0; i < 256; i++) {
        const int r = sd_bus_process(g_bus, nullptr);
        if (r < 0) {
            g_error = std::format("sd_bus_process: {}", strerror(-r));
            teardown();
            return;
        }
        if (r == 0)
            break;
    }
    rearm();
}

int onFd(int, uint32_t mask, void*) {
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        g_error = "conexão com o D-Bus caiu";
        teardown();
        return 0;
    }
    pump();
    return 0;
}

int onTimer(void*) {
    pump();
    return 0;
}

// ---------- parser do dbusmenu ----------
// item: (ia{sv}av) — id, propriedades, filhos (cada filho é uma variante com outro item)

bool parseItem(sd_bus_message* m, mb::Node& out);

bool readProps(sd_bus_message* m, mb::Node& n, bool& visible) {
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
        return false;
    std::string type, toggle, display;
    int         r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char* key = nullptr;
        if (sd_bus_message_read(m, "s", &key) < 0)
            return false;
        const char* contents = nullptr;
        char        t        = 0;
        if (sd_bus_message_peek_type(m, &t, &contents) < 0 || t != 'v' || !contents)
            return false;
        const std::string k = key ? key : "", c = contents;

        auto readStr = [&](std::string& dst) {
            const char* s = nullptr;
            if (sd_bus_message_read(m, "v", "s", &s) < 0)
                return false;
            dst = s ? s : "";
            return true;
        };
        bool ok = true;
        if (c == "s" && k == "label") {
            std::string s;
            ok      = readStr(s);
            n.label = mb::stripMnemonic(s);
        } else if (c == "s" && k == "type")
            ok = readStr(type);
        else if (c == "s" && k == "toggle-type")
            ok = readStr(toggle);
        else if (c == "s" && k == "children-display")
            ok = readStr(display);
        else if (c == "b" && (k == "enabled" || k == "visible")) {
            int b = 1;
            ok    = sd_bus_message_read(m, "v", "b", &b) >= 0;
            (k == "enabled" ? n.enabled : visible) = b;
        } else if (c == "i" && k == "toggle-state") {
            int32_t v = 0;
            ok        = sd_bus_message_read(m, "v", "i", &v) >= 0;
            n.checked = v == 1;
        } else if (c == "aas" && k == "shortcut") {
            std::vector<std::vector<std::string>> chords;
            ok = sd_bus_message_enter_container(m, 'v', "aas") >= 0 && sd_bus_message_enter_container(m, 'a', "as") >= 0;
            while (ok && sd_bus_message_at_end(m, false) == 0) {
                char** l = nullptr;
                if (sd_bus_message_read_strv(m, &l) < 0) {
                    ok = false;
                    break;
                }
                std::vector<std::string> chord;
                for (char** p = l; p && *p; p++) {
                    chord.emplace_back(*p);
                    free(*p);
                }
                free(l);
                chords.push_back(std::move(chord));
            }
            ok         = ok && sd_bus_message_exit_container(m) >= 0 && sd_bus_message_exit_container(m) >= 0;
            n.shortcut = mb::formatShortcut(chords);
        } else
            ok = sd_bus_message_skip(m, "v") >= 0;
        if (!ok || sd_bus_message_exit_container(m) < 0)
            return false;
    }
    if (r < 0 || sd_bus_message_exit_container(m) < 0)
        return false;

    n.separator = type == "separator";
    n.submenu   = display == "submenu";
    n.toggle    = toggle == "checkmark" ? mb::Toggle::Check : toggle == "radio" ? mb::Toggle::Radio : mb::Toggle::None;
    return true;
}

bool parseItem(sd_bus_message* m, mb::Node& out) {
    if (sd_bus_message_enter_container(m, 'r', "ia{sv}av") < 0)
        return false;
    int32_t id = 0;
    if (sd_bus_message_read(m, "i", &id) < 0)
        return false;
    out.id       = id;
    bool visible = true;
    if (!readProps(m, out, visible))
        return false;
    if (sd_bus_message_enter_container(m, 'a', "v") < 0)
        return false;
    int r;
    while ((r = sd_bus_message_enter_container(m, 'v', "(ia{sv}av)")) > 0) {
        mb::Node child; // a visibilidade vem dentro do filho: parseia e decide depois
        if (!parseItem(m, child))
            return false;
        if (sd_bus_message_exit_container(m) < 0)
            return false;
        if (child.id != INT32_MIN)
            out.children.push_back(std::move(child));
    }
    if (r < 0 || sd_bus_message_exit_container(m) < 0 || sd_bus_message_exit_container(m) < 0)
        return false;
    if (!out.children.empty())
        out.submenu = true;
    if (!visible)
        out.id = INT32_MIN; // marcador: o pai descarta
    return true;
}

// tira separadores no começo, no fim e repetidos (sobram quando itens são escondidos)
void tidy(mb::Node& n) {
    std::vector<mb::Node> kids;
    for (auto& c : n.children) {
        if (c.separator && (kids.empty() || kids.back().separator))
            continue;
        kids.push_back(std::move(c));
    }
    while (!kids.empty() && kids.back().separator)
        kids.pop_back();
    n.children = std::move(kids);
    for (auto& c : n.children)
        tidy(c);
}

// ---------- cliente dbusmenu ----------

void fetch();

int onLayout(sd_bus_message* m, void* ud, sd_bus_error*) {
    if (!current(ud))
        return 0;
    auto& c    = *g_client;
    c.fetching = false;
    if (sd_bus_message_is_method_error(m, nullptr)) {
        const auto* e = sd_bus_message_get_error(m);
        g_error       = std::format("GetLayout: {}", e && e->message ? e->message : "erro");
    } else {
        uint32_t rev = 0;
        mb::Node root;
        if (sd_bus_message_read(m, "u", &rev) >= 0 && parseItem(m, root)) {
            tidy(root);
            c.root   = std::move(root);
            c.loaded = true;
            changed();
        } else
            g_error = "GetLayout: resposta malformada";
    }
    if (c.again) {
        c.again = false;
        fetch();
    }
    return 0;
}

void fetch() {
    if (!g_bus || !g_client)
        return;
    auto& c = *g_client;
    if (c.fetching) { // já tem uma no caminho: busca de novo quando ela voltar
        c.again = true;
        return;
    }
    const int r = sd_bus_call_method_async(g_bus, nullptr, c.service.c_str(), c.path.c_str(), MENU_IFACE, "GetLayout", onLayout, genPtr(c.gen), "iias", 0,
                                           -1, 0);
    c.fetching  = r >= 0;
    if (r < 0)
        g_error = std::format("GetLayout: {}", strerror(-r));
    rearm();
}

int onMenuSignal(sd_bus_message*, void* ud, sd_bus_error*) {
    if (current(ud))
        fetch();
    return 0;
}

int ignoreReply(sd_bus_message*, void*, sd_bus_error*) {
    return 0;
}

int onAboutToShow(sd_bus_message* m, void* ud, sd_bus_error*) {
    if (!current(ud))
        return 0;
    // needUpdate costuma vir falso mesmo quando o app mudou os itens (e o LayoutUpdated às vezes não vem):
    // busca de novo de qualquer jeito, é barato
    (void)m;
    fetch();
    return 0;
}

void follow(uint32_t xid, const Entry& e) {
    if (g_client && g_client->xid == xid && g_client->service == e.service && g_client->path == e.path)
        return;
    g_client          = std::make_unique<Client>();
    g_client->gen     = ++g_gen;
    g_client->xid     = xid;
    g_client->service = e.service;
    g_client->path    = e.path;
    if (!g_bus)
        return;
    sd_bus_match_signal_async(g_bus, &g_client->layoutMatch, e.service.c_str(), e.path.c_str(), MENU_IFACE, "LayoutUpdated", onMenuSignal, nullptr,
                              genPtr(g_client->gen));
    sd_bus_match_signal_async(g_bus, &g_client->propsMatch, e.service.c_str(), e.path.c_str(), MENU_IFACE, "ItemsPropertiesUpdated", onMenuSignal,
                              nullptr, genPtr(g_client->gen));
    fetch();
}

void event(int id, const char* name) {
    if (!g_bus || !g_client)
        return;
    sd_bus_call_method_async(g_bus, nullptr, g_client->service.c_str(), g_client->path.c_str(), MENU_IFACE, "Event", ignoreReply, nullptr, "isvu", id,
                             name, "i", 0, (uint32_t)0);
    rearm();
}

// ---------- Registrar ----------

void forget(uint32_t xid) {
    g_windows.erase(xid);
    if (g_client && g_client->xid == xid) {
        g_client.reset();
        changed();
    }
}

int mRegister(sd_bus_message* m, void*, sd_bus_error*) {
    uint32_t    xid  = 0;
    const char* path = nullptr;
    if (int r = sd_bus_message_read(m, "uo", &xid, &path); r < 0)
        return r;
    const char* sender = sd_bus_message_get_sender(m);
    if (!sender || !path)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "sem remetente");
    g_windows[xid] = {sender, path};
    if (g_client && g_client->xid == xid)
        g_client.reset(); // registrou de novo (outro caminho): recomeça
    sd_bus_emit_signal(g_bus, REG_PATH, REG_NAME, "WindowRegistered", "uso", xid, sender, path);
    changed();
    return sd_bus_reply_method_return(m, "");
}

int mUnregister(sd_bus_message* m, void*, sd_bus_error*) {
    uint32_t xid = 0;
    if (int r = sd_bus_message_read(m, "u", &xid); r < 0)
        return r;
    forget(xid);
    sd_bus_emit_signal(g_bus, REG_PATH, REG_NAME, "WindowUnregistered", "u", xid);
    return sd_bus_reply_method_return(m, "");
}

int mGetMenu(sd_bus_message* m, void*, sd_bus_error*) {
    uint32_t xid = 0;
    if (int r = sd_bus_message_read(m, "u", &xid); r < 0)
        return r;
    const auto it = g_windows.find(xid);
    if (it == g_windows.end())
        return sd_bus_reply_method_errorf(m, "com.canonical.AppMenu.Registrar.Error.NoMenu", "janela 0x%x sem menu", xid);
    return sd_bus_reply_method_return(m, "so", it->second.service.c_str(), it->second.path.c_str());
}

int mGetMenus(sd_bus_message* m, void*, sd_bus_error*) {
    sd_bus_message* reply = nullptr;
    int             r     = sd_bus_message_new_method_return(m, &reply);
    if (r >= 0)
        r = sd_bus_message_open_container(reply, 'a', "(uso)");
    for (auto& [xid, e] : g_windows)
        if (r >= 0)
            r = sd_bus_message_append(reply, "(uso)", xid, e.service.c_str(), e.path.c_str());
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    if (r >= 0)
        r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
}

const sd_bus_vtable REG_VTABLE[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterWindow", "uo", "", mRegister, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("UnregisterWindow", "u", "", mUnregister, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetMenuForWindow", "u", "so", mGetMenu, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetMenus", "", "a(uso)", mGetMenus, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("WindowRegistered", "uso", 0),
    SD_BUS_SIGNAL("WindowUnregistered", "u", 0),
    SD_BUS_VTABLE_END,
};

// um app saiu do bus: as janelas dele não têm mais menu
int onNameOwnerChanged(sd_bus_message* m, void*, sd_bus_error*) {
    const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &oldOwner, &newOwner) < 0 || !name)
        return 0;
    if (std::string(name) == REG_NAME) {
        const char* me = nullptr;
        g_owner        = newOwner && sd_bus_get_unique_name(g_bus, &me) >= 0 && me && std::string(newOwner) == me;
        return 0;
    }
    if (newOwner && *newOwner)
        return 0;
    std::vector<uint32_t> gone;
    for (auto& [xid, e] : g_windows)
        if (e.service == name)
            gone.push_back(xid);
    for (auto xid : gone)
        forget(xid);
    return 0;
}

int onNameAcquired(sd_bus_message* m, void*, sd_bus_error*) {
    const char* name = nullptr;
    if (sd_bus_message_read(m, "s", &name) >= 0 && name && std::string(name) == REG_NAME)
        g_owner = true;
    return 0;
}

int onNameLost(sd_bus_message* m, void*, sd_bus_error*) {
    const char* name = nullptr;
    if (sd_bus_message_read(m, "s", &name) >= 0 && name && std::string(name) == REG_NAME)
        g_owner = false;
    return 0;
}

int onRequestName(sd_bus_message* m, void*, sd_bus_error*) {
    if (sd_bus_message_is_method_error(m, nullptr)) {
        const auto* e = sd_bus_message_get_error(m);
        g_error       = std::format("RequestName: {}", e && e->message ? e->message : "erro");
    }
    return 0;
}

void teardown() {
    g_client.reset();
    for (auto* s : g_slots)
        sd_bus_slot_unref(s);
    g_slots.clear();
    if (g_fdSrc)
        wl_event_source_remove(g_fdSrc);
    if (g_timerSrc)
        wl_event_source_remove(g_timerSrc);
    g_fdSrc = g_timerSrc = nullptr;
    if (g_bus) {
        sd_bus_flush_close_unref(g_bus);
        g_bus = nullptr;
    }
    g_owner = false;
    g_windows.clear();
}

} // namespace

bool start(wl_event_loop* loop, void (*onChange)()) {
    g_onChange = onChange;
    g_error.clear();
    if (int r = sd_bus_open_user_with_description(&g_bus, "hyprmenu"); r < 0) {
        g_error = std::format("sem D-Bus de sessão: {}", strerror(-r));
        g_bus   = nullptr;
        return false;
    }
    g_fdSrc    = wl_event_loop_add_fd(loop, sd_bus_get_fd(g_bus), WL_EVENT_READABLE, onFd, nullptr);
    g_timerSrc = wl_event_loop_add_timer(loop, onTimer, nullptr);
    if (!g_fdSrc || !g_timerSrc) {
        g_error = "não deu para ligar o D-Bus ao event loop";
        teardown();
        return false;
    }

    sd_bus_slot* s = nullptr;
    if (sd_bus_add_object_vtable(g_bus, &s, REG_PATH, REG_NAME, REG_VTABLE, nullptr) >= 0)
        g_slots.push_back(s);
    s = nullptr;
    if (sd_bus_match_signal_async(g_bus, &s, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                                  onNameOwnerChanged, nullptr, nullptr) >= 0)
        g_slots.push_back(s);
    s = nullptr;
    if (sd_bus_match_signal_async(g_bus, &s, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameAcquired", onNameAcquired,
                                  nullptr, nullptr) >= 0)
        g_slots.push_back(s);
    s = nullptr;
    if (sd_bus_match_signal_async(g_bus, &s, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameLost", onNameLost, nullptr,
                                  nullptr) >= 0)
        g_slots.push_back(s);
    s = nullptr;
    // se outro Registrar já existe, entra na fila e assume quando ele sair
    if (sd_bus_request_name_async(g_bus, &s, REG_NAME, SD_BUS_NAME_QUEUE, onRequestName, nullptr) >= 0)
        g_slots.push_back(s);
    rearm();
    return true;
}

void stop() {
    teardown();
    g_onChange = nullptr;
}

const mb::Node* menuFor(uint32_t xid) {
    const auto it = xid ? g_windows.find(xid) : g_windows.end();
    if (it == g_windows.end()) {
        g_client.reset();
        return nullptr;
    }
    follow(xid, it->second);
    return g_client && g_client->loaded ? &g_client->root : nullptr;
}

void opened(int id) {
    if (!g_bus || !g_client)
        return;
    event(id, "opened");
    sd_bus_call_method_async(g_bus, nullptr, g_client->service.c_str(), g_client->path.c_str(), MENU_IFACE, "AboutToShow", onAboutToShow,
                             genPtr(g_client->gen), "i", id);
    rearm();
}

void closed(int id) {
    event(id, "closed");
}

void activate(int id) {
    event(id, "clicked");
}

std::string status() {
    std::string s = !g_bus ? "D-Bus: desligado" : std::format("registrar: {}, {} janela(s) com menu", g_owner ? "sim" : "não (outro programa é)", g_windows.size());
    if (g_client)
        s += std::format("\nacompanhando: 0x{:x} {} {} ({}, {} menus)", g_client->xid, g_client->service, g_client->path,
                         g_client->loaded ? "carregado" : "carregando", g_client->root.children.size());
    if (!g_error.empty())
        s += "\núltimo erro: " + g_error;
    return s;
}

} // namespace appmenu
