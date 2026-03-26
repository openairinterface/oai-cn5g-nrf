// h2_request.hpp — declaration only.
// Method bodies are in h2_request.cpp which includes h2_connection.hpp
// for the complete StreamData definition.
#pragma once
#include <map>
#include <string>

namespace h2 {

struct StreamData;  // forward declaration — complete type in h2_connection.hpp

struct UriRef {
  std::string scheme;
  std::string host;
  std::string path;
  std::string raw_query;
};

class Request {
 public:
  // Constructor declared only — defined in h2_request.cpp where StreamData
  // is a complete type.
  explicit Request(const StreamData& strm);

  const std::string& method() const { return method_; }
  const UriRef& uri() const { return uri_; }
  const std::multimap<std::string, std::string>& header() const { return headers_; }
  const std::string& body() const { return body_; }

 private:
  std::string method_;
  UriRef uri_;
  std::multimap<std::string, std::string> headers_;
  std::string body_;
};

}  // namespace h2
