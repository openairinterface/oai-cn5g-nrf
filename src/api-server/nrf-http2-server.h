/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License. You may obtain a copy of the
 * License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#ifndef FILE_NRF_HTTP2_SERVER_SEEN
#define FILE_NRF_HTTP2_SERVER_SEEN

#include <atomic>
#define USE_NATIVE_HTTP2 1
#ifdef USE_NATIVE_HTTP2
#include "h2_server.hpp"
#include "h2_request.hpp"
#include "h2_response.hpp"
#else
//#include "nrf.h"
#include <nghttp2/asio_http2_server.h>
#endif

#include "nrf_app.hpp"
#include "uint_generator.hpp"

using namespace oai::model::nrf;
using namespace oai::nrf::app;

#ifdef USE_NATIVE_HTTP2
// Type aliases for backward compatibility with handler code
using header_map   = h2::HeaderMap;
using header_value = h2::HeaderValue;
#else
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;
#endif

class nrf_http2_server {
 public:
  nrf_http2_server(std::string addr, uint32_t port, nrf_app* nrf_app_inst)
      : m_address(addr),
        m_port(port),
        m_nrf_app(nrf_app_inst),
        m_running(false) {}
  void start();
  void init(size_t thr);

#ifdef USE_NATIVE_HTTP2
  void register_nf_instance_handler(
      const NFProfile& NFProfiledata, h2::Response& response);
  void deregister_nf_instance_handler(
      const std::string& nfInstanceID, h2::Response& response);
  void get_nf_instance_handler(
      const std::string& nfInstanceID, h2::Response& response);
  void get_nf_instances_handler(
      const std::string& nf_type, const std::string& limit_nfs,
      h2::Response& response);
  void update_instance_handler(
      const std::string& nfInstanceID,
      const std::vector<oai::model::common::PatchItem>& patchItem,
      h2::Response& response);
  void create_subscription_handler(
      const SubscriptionData& subscriptionData, h2::Response& response);
  void update_subscription_handler(
      const std::string& subscriptionID,
      const std::vector<oai::model::common::PatchItem>& patchItem,
      h2::Response& response);
  void remove_subscription_handler(
      const std::string& subscriptionID, h2::Response& response);
  void search_nf_instances_handler(
      const std::string& target_nf_type,
      const std::string& requester_nf_type,
      const std::string& requester_nf_instance_id,
      const std::string& limit_nfs,
      h2::Response& response);
  void access_token_request_handler(
      const SubscriptionData& subscriptionData, h2::Response& response);
#else
  // Original handler signatures with nghttp2-asio types
  void register_nf_instance_handler(
      const NFProfile& NFProfiledata, const response& response);
  void deregister_nf_instance_handler(
      const std::string& nfInstanceID, const response& response);
  void get_nf_instance_handler(
      const std::string& nfInstanceID, const response& response);
  void get_nf_instances_handler(
      const std::string& nf_type, const std::string& limit_nfs,
      const response& response);
  void update_instance_handler(
      const std::string& nfInstanceID,
      const std::vector<oai::model::common::PatchItem>& patchItem,
      const response& response);
  void create_subscription_handler(
      const SubscriptionData& subscriptionData, const response& response);
  void update_subscription_handler(
      const std::string& subscriptionID,
      const std::vector<oai::model::common::PatchItem>& patchItem,
      const response& response);
  void remove_subscription_handler(
      const std::string& subscriptionID, const response& response);
  void search_nf_instances_handler(
      const std::string& target_nf_type, const std::string& requester_nf_type,
      const std::string& requester_nf_instance_id,
      const std::string& limit_nfs, const response& response);
  void access_token_request_handler(
      const SubscriptionData& subscriptionData, const response& response);
#endif
  void stop();

 private:
  oai::utils::uint_generator<uint32_t> m_promise_id_generator;
  std::string m_address;
  uint32_t m_port;
#ifdef USE_NATIVE_HTTP2
  h2::Http2Server m_server;
#else
  http2 server;
#endif
  nrf_app* m_nrf_app;
  std::atomic<bool> m_running;  // Fixes UB: was uninitialized bool

 protected:
  static uint64_t generate_promise_id() {
    return oai::utils::uint_uid_generator<uint64_t>::get_instance().get_uid();
  }
};

#endif
