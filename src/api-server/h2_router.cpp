// h2_router.cpp — implementation of h2::Router.
// Ports nghttp2-asio's serve_mux longest-prefix matching to the native h2 layer.
#include "h2_router.hpp"
#include "h2_request.hpp"
#include "h2_response.hpp"

namespace h2 {

// Register a handler for the given path pattern.
//
// Rules:
//   * Rejects empty patterns and null handlers.
//   * Returns false (no-op) if the pattern is already user-defined.
//   * For trailing-slash patterns ("/foo/") installs an automatic 301-redirect
//     from the stripped path ("/foo") unless that path is already user-defined.
//   * Uses insert_or_assign so a prior auto-redirect entry (user_defined=false)
//     for the same key is always replaced by a user-defined entry.
bool Router::handle(std::string pattern, RequestHandler handler) {
  if (pattern.empty() || !handler) return false;

  auto it = routes_.find(pattern);
  if (it != routes_.end() && it->second.user_defined) return false;

  if (pattern.size() >= 2 && pattern.back() == '/') {
    // /foo/ was registered → auto-install 301-redirect for /foo unless /foo
    // is already claimed by the user.
    auto redirect = pattern.substr(0, pattern.size() - 1);
    auto rit = routes_.find(redirect);
    if (rit == routes_.end() || !rit->second.user_defined) {
      auto redir_handler = [loc = pattern](const Request& /*req*/, Response& res) {
        h2::HeaderMap h;
        h.emplace("location", HeaderValue{loc});
        res.write_head(301, h);
        res.end();
      };
      routes_.insert_or_assign(redirect, Entry{false, redir_handler, pattern});
    }
  }

  routes_.insert_or_assign(pattern, Entry{true, std::move(handler), pattern});
  return true;
}

// Return the handler whose pattern best matches `path`.
//
// Matching rules:
//   * Non-slash-terminated patterns: exact match only.
//   * Slash-terminated patterns (prefix): match if path starts with the pattern.
//   * Among all matching entries, the one with the longest pattern wins
//     (longest-prefix / most-specific wins).
//
// Returns nullptr if no pattern matched.
RequestHandler Router::match(const std::string& path) const {
  const Entry* best = nullptr;
  size_t best_len = 0;

  for (const auto& [pattern, entry] : routes_) {
    bool matches;
    if (pattern.back() != '/') {
      // Exact match.
      matches = (pattern == path);
    } else {
      // Prefix match: path must be at least as long as the pattern and start
      // with the pattern string (which ends in '/').
      matches = path.size() >= pattern.size() &&
                path.substr(0, pattern.size()) == pattern;
    }

    if (matches && (!best || pattern.size() > best_len)) {
      best     = &entry;
      best_len = pattern.size();
    }
  }

  return best ? best->handler : nullptr;
}

}  // namespace h2
