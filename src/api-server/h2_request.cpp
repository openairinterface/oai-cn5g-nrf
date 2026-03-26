// h2_request.cpp — implementation of h2::Request.
// Includes h2_connection.hpp to get the complete StreamData definition.
// Must NOT be included from h2_request.hpp — would re-introduce the
// incomplete-type problem that splitting was designed to avoid.
#include "h2_request.hpp"
#include "h2_connection.hpp"  // provides complete StreamData definition

namespace h2 {

Request::Request(const StreamData& strm)
    : method_(strm.method), body_(strm.body) {
  uri_.scheme    = strm.scheme;
  uri_.host      = strm.authority;
  uri_.path      = strm.path;
  uri_.raw_query = strm.raw_query;
  headers_       = strm.headers;
}

}  // namespace h2
