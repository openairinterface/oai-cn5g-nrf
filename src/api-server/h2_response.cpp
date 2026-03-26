// h2_response.cpp — implementation of h2::Response.
// Includes h2_connection.hpp to get the complete Connection definition,
// enabling the call to conn_->submit_response() in end().
#include "h2_response.hpp"
#include "h2_connection.hpp"  // provides complete Connection definition

namespace h2 {

Response::Response(std::shared_ptr<Connection> conn, int32_t stream_id)
    : conn_(std::move(conn)), stream_id_(stream_id) {}

void Response::write_head(unsigned int status_code) {
  status_code_ = status_code;
  head_sent_   = true;
}

void Response::write_head(unsigned int status_code, const HeaderMap& headers) {
  status_code_ = status_code;
  for (const auto& [k, hv] : headers) {
    headers_.emplace(k, hv.value);
  }
  head_sent_ = true;
}

void Response::end(std::string body) {
  if (!head_sent_) write_head(200);
  conn_->submit_response(stream_id_,
                         static_cast<uint16_t>(status_code_),
                         headers_,
                         std::move(body));
}

void Response::end() { end(std::string{}); }

}  // namespace h2
