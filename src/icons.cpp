// Resolução de ícones: classe da janela -> .desktop -> arquivo no tema -> surface cairo.
// Sem dependências do Hyprland. SVG só com -DHYPRTAB_RSVG (librsvg).

#include "icons.hpp"

#include <pango/pangocairo.h>
#ifdef HYPRTAB_RSVG
#include <librsvg/rsvg.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// classe (minúscula) -> dados do .desktop. Indexado uma vez.
const std::unordered_map<std::string, icons::DesktopEntry>& desktopIndex() {
    static const auto idx = [] {
        std::unordered_map<std::string, icons::DesktopEntry> m;
        std::vector<fs::path>                        dirs = {"/usr/share/applications", "/usr/local/share/applications",
                                                             "/var/lib/flatpak/exports/share/applications"};
        if (const char* home = getenv("HOME")) {
            dirs.push_back(fs::path(home) / ".local/share/applications");
            dirs.push_back(fs::path(home) / ".local/share/flatpak/exports/share/applications");
        }
        for (auto& d : dirs) {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(d, ec)) {
                if (e.path().extension() != ".desktop")
                    continue;
                std::ifstream        f(e.path());
                std::string          line, wm;
                icons::DesktopEntry  de;
                bool                 main = false; // só a seção [Desktop Entry]; as [Desktop Action] vêm depois
                while (std::getline(f, line)) {
                    if (line.starts_with('[')) {
                        main = line.starts_with("[Desktop Entry]");
                        continue;
                    }
                    if (!main)
                        continue;
                    if (line.starts_with("Icon=") && de.icon.empty())
                        de.icon = line.substr(5);
                    else if (line.starts_with("Exec=") && de.exec.empty())
                        de.exec = line.substr(5);
                    else if (line.starts_with("Name=") && de.name.empty())
                        de.name = line.substr(5);
                    else if (line.starts_with("StartupWMClass="))
                        wm = line.substr(15);
                }
                if (de.icon.empty() && de.exec.empty())
                    continue;
                const auto stem = lower(e.path().stem().string());
                if (!wm.empty())
                    m.emplace(lower(wm), de);
                m.emplace(stem, de);
                if (auto dot = stem.rfind('.'); dot != std::string::npos)
                    m.emplace(stem.substr(dot + 1), de);
            }
        }
        return m;
    }();
    return idx;
}

// Procura o arquivo sem varrer a árvore inteira: raiz/tema/tamanho/{apps,applications}/nome.ext
std::string findFile(const std::string& name) {
    if (name.empty())
        return "";
    if (name[0] == '/')
        return fs::exists(name) ? name : "";

    std::vector<fs::path> roots = {"/usr/share/icons", "/usr/local/share/icons", "/var/lib/flatpak/exports/share/icons"};
    if (const char* home = getenv("HOME"))
        roots.push_back(fs::path(home) / ".local/share/icons");

    std::string bestPng, svg;
    int         bestSize = -1;

    auto consider = [&](const fs::path& dir, int size) {
        std::error_code ec;
        if (fs::exists(dir / (name + ".png"), ec) && size > bestSize) {
            bestSize = size;
            bestPng  = (dir / (name + ".png")).string();
        }
        if (svg.empty() && fs::exists(dir / (name + ".svg"), ec))
            svg = (dir / (name + ".svg")).string();
    };

    for (auto& root : roots) {
        std::error_code ec;
        for (auto& theme : fs::directory_iterator(root, ec)) {
            if (!theme.is_directory(ec))
                continue;
            for (auto& sz : fs::directory_iterator(theme.path(), ec)) {
                if (!sz.is_directory(ec))
                    continue;
                const auto s    = sz.path().filename().string();
                const int  size = (!s.empty() && isdigit((unsigned char)s[0])) ? atoi(s.c_str()) : (s == "scalable" ? 0 : -1);
                for (auto* sub : {"apps", "applications"})
                    consider(sz.path() / sub, size);
                // layout alternativo: tema/apps/<tamanho>/nome
                if (s == "apps")
                    for (auto& inner : fs::directory_iterator(sz.path(), ec))
                        if (inner.is_directory(ec)) {
                            const auto n = inner.path().filename().string();
                            consider(inner.path(), (!n.empty() && isdigit((unsigned char)n[0])) ? atoi(n.c_str()) : 0);
                        }
            }
        }
    }
    for (auto* p : {"/usr/share/pixmaps"}) {
        consider(p, 1);
    }

#ifdef HYPRTAB_RSVG
    if (!svg.empty())
        return svg; // vetorial: nítido em qualquer escala
#endif
    return bestPng;
}

bool drawFile(cairo_t* cr, const std::string& path, int px) {
    if (path.ends_with(".png")) {
        cairo_surface_t* png = cairo_image_surface_create_from_png(path.c_str());
        bool             ok  = cairo_surface_status(png) == CAIRO_STATUS_SUCCESS;
        if (ok) {
            const double s = (double)px / std::max(cairo_image_surface_get_width(png), cairo_image_surface_get_height(png));
            cairo_save(cr);
            cairo_scale(cr, s, s);
            cairo_set_source_surface(cr, png, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        cairo_surface_destroy(png);
        return ok;
    }
#ifdef HYPRTAB_RSVG
    if (path.ends_with(".svg")) {
        GError*     err = nullptr;
        RsvgHandle* h   = rsvg_handle_new_from_file(path.c_str(), &err);
        if (!h) {
            g_clear_error(&err);
            return false;
        }
        RsvgRectangle vp{0, 0, (double)px, (double)px};
        const bool    ok = rsvg_handle_render_document(h, cr, &vp, nullptr);
        g_object_unref(h);
        return ok;
    }
#endif
    return false;
}

void drawFallback(cairo_t* cr, const std::string& cls, int px) {
    cairo_arc(cr, px / 2.0, px / 2.0, px / 2.0 - px * 0.04, 0, 6.2832);
    cairo_set_source_rgba(cr, 0.35, 0.45, 0.8, 1);
    cairo_fill(cr);

    auto* layout = pango_cairo_create_layout(cr);
    auto* fd     = pango_font_description_from_string(("Sans Bold " + std::to_string(std::max(8, px * 4 / 10))).c_str());
    pango_layout_set_font_description(layout, fd);
    const std::string letter = cls.empty() ? "?" : std::string(1, (char)toupper((unsigned char)cls[0]));
    pango_layout_set_text(layout, letter.c_str(), -1);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_move_to(cr, (px - w) / 2.0, (px - h) / 2.0);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(fd);
    g_object_unref(layout);
}

} // namespace

namespace icons {

cairo_surface_t* load(const std::string& windowClass, int px) {
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t*         cr   = cairo_create(surf);

    const auto key = lower(windowClass);
    std::string name = key;
    if (auto it = desktopIndex().find(key); it != desktopIndex().end() && !it->second.icon.empty())
        name = it->second.icon;

    bool ok = false;
    if (const auto file = findFile(name); !file.empty())
        ok = drawFile(cr, file, px);
    if (!ok)
        drawFallback(cr, windowClass, px);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    return surf;
}

const DesktopEntry* desktopEntry(const std::string& windowClass) {
    const auto it = desktopIndex().find(lower(windowClass));
    return it == desktopIndex().end() ? nullptr : &it->second;
}

} // namespace icons
