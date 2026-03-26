// h2_router.hpp — declaration only.
// Method bodies are in h2_router.cpp.
#pragma once
#include <functional>
#include <map>
#include <string>

namespace h2 {

class Request;
class Response;

using RequestHandler = std::function<void(const Request&, Response&)>;

class Router {
 public:
  bool handle(std::string pattern, RequestHandler handler);
  RequestHandler match(const std::string& path) const;

 private:
  struct Entry {
    bool user_defined = false;
    RequestHandler handler;
    std::string pattern;
  };
  std::map<std::string, Entry> routes_;
};

}  // namespace h2
