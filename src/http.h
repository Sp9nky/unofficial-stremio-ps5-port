#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
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

// Any method, with a raw body or a multipart form: for the debrid services' APIs (src/debrid.cpp). Blocking.
struct HttpRequest {
	std::string method = "GET";
	std::string url;
	std::vector<std::string> headers;                         // "Name: value"
	std::string body;                                         // sent as is, with content_type
	std::string content_type;
	std::vector<std::pair<std::string, std::string>> form;    // multipart/form-data fields, instead of a body
	long timeout_s = 25;
	const std::atomic<bool>* cancel = nullptr;
};
HttpResponse http_request(const HttpRequest& request);

// GET requests that keep their connections open between calls (HTTP keep-alive), for one thread: the artwork workers
// fetch hundreds of pictures from the same few hosts, and a new TLS connection each time cost from a quarter of a
// second to over a second per picture (images.metahub.space and its live.metahub.space redirect, measured).
class HttpSession {
public:
	HttpSession() = default;
	~HttpSession();
	HttpSession(const HttpSession&) = delete;
	HttpSession& operator=(const HttpSession&) = delete;
	HttpResponse get(const std::string& url, long timeout_s = 20, const std::vector<std::string>& headers = {});

private:
	void* curl_ = nullptr;
};

// A large download, handed on piece by piece: `sink` returns false to stop (the request then ends with the
// error "stopped"). The response has no body; a 200 status and no error mean it all arrived.
HttpResponse http_download(const std::string& url, const std::function<bool(const char*, size_t)>& sink,
                           const std::atomic<bool>* cancel = nullptr, const std::vector<std::string>& headers = {});

const std::string& http_ca_bundle();
// What every curl handle needs on this platform (a CURL*). In the native PS5
// app: sockets made non-blocking with the console's own option.
void http_setup_handle(void* curl);
extern const char* kUserAgent;
