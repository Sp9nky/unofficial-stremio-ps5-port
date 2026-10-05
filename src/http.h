#pragma once

#include <atomic>
#include <string>
#include <vector>

struct HttpResponse {
	long status = 0;       // HTTP status, 0 when the request never got an answer
	std::string body;
	std::string error;     // transport error (DNS, TLS, timeout, ...)
	std::string content_type;
	bool ok() const { return error.empty() && status >= 200 && status < 300; }
	std::string describe() const;  // "HTTP 404" / "Could not resolve host: ..."
};

void http_init(const std::string& ca_bundle_path);

// Blocking requests, for worker threads. `cancel` may be flipped to abort.
HttpResponse http_get(const std::string& url, long timeout_s = 20, const std::atomic<bool>* cancel = nullptr,
                      const std::vector<std::string>& headers = {});
HttpResponse http_post_json(const std::string& url, const std::string& body, long timeout_s = 20,
                            const std::atomic<bool>* cancel = nullptr);

const std::string& http_ca_bundle();
// What every curl handle needs on this platform (a CURL*). In the native PS5
// app: sockets made non-blocking with the console's own option.
void http_setup_handle(void* curl);
extern const char* kUserAgent;
