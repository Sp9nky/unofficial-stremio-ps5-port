// Typing with the console's keyboard, and searching with it.

#include "session.hpp"

#include <algorithm>

namespace sx
{

namespace
{

constexpr std::size_t kMaxRowCards = 50;

std::string row_title(const Catalog &c)
{
    const std::string type = capitalize(c.type);
    return c.name.empty() ? type : c.name + " - " + type;
}

} // namespace

void Session::ask_text(const std::function<void(const std::string &)> &done)
{
    text_done_ = done;
    if (!ime_.open())
    {
        text_done_ = nullptr;
        say("The keyboard could not be opened.", 4.0, true);
    }
}

void Session::poll_typing()
{
    if (!ime_.active())
        return;
    std::string text;
    const Ime::State s = ime_.poll(&text);
    if (s == Ime::State::done)
    {
        const auto fn = text_done_;
        text_done_ = nullptr;
        if (fn)
            fn(text);
    }
    else if (s == Ime::State::cancelled)
    {
        text_done_ = nullptr;
    }
}

void Session::begin_search()
{
    ask_text([this](const std::string &q) { start_search(q); });
}

void Session::close_search()
{
    ++search_gen_;
    search_open = false;
    search.rows.clear();
    search.status.clear();
    search_.clear();
    search_map_.clear();
    changed_ |= kSearch | kSearchNew;
}

void Session::start_search(const std::string &text)
{
    const std::string q = trim(text);
    if (q.empty())
        return;
    const int gen = ++search_gen_;
    search_query = q;
    search_open = true;
    search_.clear();
    search_map_.clear();
    search.rows.clear();
    for (const auto &a : addons_)
        for (const auto &c : a->catalogs)
        {
            if (!c.has_extra("search") || c.requires_other_than("search"))
                continue;
            BoardRow row;
            row.key = "search|" + a->transport_url + "|" + c.type + "|" + c.id;
            row.title = row_title(c);
            row.addon_url = a->transport_url;
            row.catalog = c;
            search_.push_back(row);
        }
    changed_ |= kSearch | kSearchNew;
    if (search_.empty())
    {
        search.status = addons_loading_ ? "Addons are still loading..." : "None of your addons can search.";
        return;
    }
    search.status = "Searching...";
    search_pending_ = static_cast<int>(search_.size());
    for (std::size_t i = 0; i < search_.size(); ++i)
    {
        const ::Addon *a = find_addon(search_[i].addon_url);
        const std::string url =
            a ? a->resource_url("catalog", search_[i].catalog.type, search_[i].catalog.id, "search=" + url_encode(q)) : "";
        struct Res
        {
            std::vector<Item> items;
            std::string error;
        };
        bg<Res>(
            [url]() {
                Res r;
                json j;
                if (!url.empty() && fetch_json(url, j, r.error))
                {
                    const json &metas = jobj(j, "metas");
                    if (metas.is_array())
                        for (const auto &m : metas)
                        {
                            Item it = item_from_meta_json(m);
                            if (!it.id.empty() && !it.name.empty())
                                r.items.push_back(it);
                        }
                }
                return r;
            },
            [this, gen, i](Res &r) {
                if (gen != search_gen_ || i >= search_.size())
                    return;
                search_[i].loaded = true;
                search_[i].items = std::move(r.items);
                if (search_[i].items.size() > kMaxRowCards)
                    search_[i].items.resize(kMaxRowCards);
                --search_pending_;
                refresh_search();
            });
    }
}

void Session::refresh_search()
{
    search.rows.clear();
    search_map_.clear();
    std::size_t total = 0;
    for (std::size_t i = 0; i < search_.size(); ++i)
    {
        const BoardRow &br = search_[i];
        if (!br.loaded || br.items.empty())
            continue;
        Row row;
        row.key = br.key;
        row.title = br.title;
        for (const Item &it : br.items)
            row.items.push_back(make_title(it, false));
        total += br.items.size();
        search.rows.push_back(std::move(row));
        search_map_.push_back(static_cast<int>(i));
    }
    // "No results" is the screen's own message once the search is over.
    search.status = search_pending_ > 0 && total == 0 ? "Searching..." : "";
    changed_ |= kSearch;
    art_dirty_ = true;
}

} // namespace sx
