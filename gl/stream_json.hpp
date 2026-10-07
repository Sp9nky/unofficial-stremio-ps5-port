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
    strings("request_headers", s.request_headers);
    s.filename = jstr(j, "filename");
    s.binge_group = jstr(j, "binge_group");
    s.not_web_ready = jbool(j, "not_web_ready", false);
    return s;
}