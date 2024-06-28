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

#ifndef FILE_NRF_CLIENT_HPP_SEEN
#define FILE_NRF_CLIENT_HPP_SEEN

#include "nrf_profile.hpp"

namespace oai {
namespace nrf {
namespace app {

class nrf_client {
 public:
  nrf_client();
  virtual ~nrf_client();

  nrf_client(nrf_client const&) = delete;
  void operator=(nrf_client const&) = delete;

  /*
   * Send Notification for the associated event to the subscribers
   * @param [const std::shared_ptr<nrf_profile> &] profile: NF profile
   * @param [const uint8_t &] event_type: notification type
   * @param [const std::vector<std::string> &] uris: list of subscribed NFs' URI
   * @return void
   */
  void notify_subscribed_event(
      const std::shared_ptr<nrf_profile>& profile, const uint8_t& event_type,
      const std::vector<std::string>& uris, uint8_t http_version);
};
}  // namespace app
}  // namespace nrf
}  // namespace oai
#endif /* FILE_NRF_CLIENT_HPP_SEEN */
