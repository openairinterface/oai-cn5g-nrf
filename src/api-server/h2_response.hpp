// h2_response.hpp — declaration only.
// Method bodies are in h2_response.cpp which includes h2_connection.hpp
// for the complete Connection definition.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace h2 {

class Connection;  // forward declaration — complete type in h2_connection.hpp

struct HeaderValue {
  std::string value;
  bool sensitive = false;
};

using HeaderMap = std::multimap<std::string, HeaderValue>;

class Response {
 public:
  // Constructor and all methods declared only — defined in h2_response.cpp.
  Response(std::shared_ptr<Connection> conn, int32_t stream_id);

  void write_head(unsigned int status_code);
  void write_head(unsigned int status_code, const HeaderMap& headers);
  void end();
  void end(std::string body);

 private:
  std::shared_ptr<Connection> conn_;
  int32_t stream_id_;
  unsigned int status_code_ = 200;
  std::multimap<std::string, std::string> headers_;
  bool head_sent_ = false;
};

}  // namespace h2
