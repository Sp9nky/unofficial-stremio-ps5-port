// A stream (what was playing) as JSON, so that Continue Watching can play it again.

#pragma once

#include "stremio.h"
#include "util.h"

#include <string>
#include <vector>

inline json stream_to_json(const ::Stream &s)
{
    json j;
    j["name"] = s.name;
    j["description"] = s.description;
    j["addon"] = s.addon;
    j["url"] = s.url;
    j["info_hash"] = s.info_hash;
    j["file_idx"] = s.file_idx;
    j["sources"] = s.sources;
    j["title"] = s.title;
    if (s.client_resolve.present)
    {
        const ::ClientResolve &c = s.client_resolve;
        json cr{{"type", c.type},   {"service", c.service},         {"infoHash", c.info_hash}, {"magnetUri", c.magnet_uri},
                {"torrentName", c.torrent_name}, {"filename", c.filename}, {"sources", c.sources},
                {"isCached", c.is_cached}};
        if (c.has_file_idx)
            cr["fileIdx"] = c.file_idx;
        if (c.season >= 0)
            cr["season"] = c.season;
        if (c.episode >= 0)
            cr["episode"] = c.episode;
        j["client_resolve"] = cr;
    }
    j["file_must_include"] = s.file_must_include;
    j["filename"] = s.filename;
    j["binge_group"] = s.binge_group;
    j["not_web_ready"] = s.not_web_ready;
    j["request_headers"] = s.request_headers;
    return j;
}

inline ::Stream stream_from_json(const json &j)
{
    ::Stream s;
    s.name = jstr(j, "name");
    s.description = jstr(j, "description");
    s.addon = jstr(j, "addon");
    s.url = jstr(j, "url");
    s.info_hash = jstr(j, "info_hash");
    s.file_idx = j.contains("file_idx") && j["file_idx"].is_number_integer() ? j["file_idx"].get<int>() : -1;
    const auto strings = [&](const char *key, std::vector<std::string> &out) {
        if (j.contains(key) && j[key].is_array())
            for (const auto &v : j[key])
                if (v.is_string())
                    out.push_back(v.get<std::string>());
    };
    strings("sources", s.sources);
    s.title = jstr(j, "title");
    if (j.contains("client_resolve") && j["client_resolve"].is_object())
    {
        const json &cr = j["client_resolve"];
        ::ClientResolve &c = s.client_resolve;
        c.present = true;
        c.type = jstr(cr, "type");
        c.service = jstr(cr, "service");
        c.info_hash = jstr(cr, "infoHash");
        c.magnet_uri = jstr(cr, "magnetUri");
        c.torrent_name = jstr(cr, "torrentName");
        c.filename = jstr(cr, "filename");
        if (cr.contains("fileIdx") && cr["fileIdx"].is_number())
        {
            c.file_idx = static_cast<int>(jnum(cr, "fileIdx", -1));
            c.has_file_idx = true;
        }
        if (cr.contains("sources") && cr["sources"].is_array())
            for (const auto &v : cr["sources"])
                if (v.is_string())
                    c.sources.push_back(v.get<std::string>());
        if (cr.contains("season") && cr["season"].is_number())
            c.season = static_cast<int>(jnum(cr, "season"));
        if (cr.contains("episode") && cr["episode"].is_number())
            c.episode = static_cast<int>(jnum(cr, "episode"));
        c.is_cached = jbool(cr, "isCached");
    }
    strings("file_must_include", s.file_must_include);
    strings("request_headers", s.request_headers);
    s.filename = jstr(j, "filename");
    s.binge_group = jstr(j, "binge_group");
    s.not_web_ready = jbool(j, "not_web_ready", false);
    return s;
}