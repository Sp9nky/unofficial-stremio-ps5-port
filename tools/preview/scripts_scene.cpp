// A sheet of titles in different scripts, to check what the interface fonts
// can draw. Titles come from addons in any language.

#include "chrome.hpp"
#include "page.hpp"
#include "scripts_scene.hpp"

namespace sx::preview
{

void draw_scripts(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop, const hui::ui::Fonts &fonts)
{
    struct Row
    {
        const char *label;
        const char *text;
    };
    const Row rows[] = {
        {"Latin", "Amélie · Pokémon: Détective Pikachu"},
        {"Latin", "Łódź – Çağlayan · İstanbul · Ångström"},
        {"Spanish", "El señor de los anillos: La comunidad"},
        {"German", "Über die Brücke – Größenwahn"},
        {"Greek", "Ο Νονός · Αγάπη στα χρόνια της χολέρας"},
        {"Cyrillic", "Белые ночи · Властелин колец: Братство Кольца"},
        {"Ukrainian", "Майстер і Маргарита"},
        {"Arabic", "الأب الروحي · سيد الخواتم: رفقة الخاتم"},
        {"Mixed", "Matrix – ماتريكس (1999)"},
        {"Mixed", "Hellas Γεια σου Κόσμε 2024"},
        {"Japanese", "千と千尋の神隠し"},
        {"Hebrew", "הסנדק"},
        {"Long title", "The Extraordinarily Long Title of a Film That Nobody Asked For: Part II"},
    };
    apply_page_backdrop(backdrop, 0.0f);
    float y = 96.0f;
    for (const Row &row : rows)
    {
        hui::ui::text(list, fonts.regular, row.label, 110.0f, y, 22.0f, theme::ink_3);
        hui::ui::text(list, fonts.semibold, fonts.semibold.font->fit(row.text, 38.0f, 760.0f), 300.0f, y + 2.0f, 38.0f,
                      theme::ink);
        hui::ui::text(list, fonts.display, fonts.display.font->fit(row.text, 36.0f, 760.0f), 1100.0f, y + 2.0f, 36.0f,
                      theme::ink_2, hui::gfx::Align::left, -0.6f);
        y += 74.0f;
    }
}

} // namespace sx::preview
