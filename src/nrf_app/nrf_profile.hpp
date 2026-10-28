/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NRF_PROFILE_HPP_SEEN
#define FILE_NRF_PROFILE_HPP_SEEN

#include <arpa/inet.h>
#include <netinet/in.h>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "3gpp_29.510.h"
#include "logger.hpp"
#include "nf_profile.hpp"
#include "nrf.h"
#include "nrf_event.hpp"

namespace oai {
namespace nrf {
namespace app {

using namespace std;
using namespace oai::common::sbi;

class nrf_profile : public oai::sba::nf_profile {
 public:
  nrf_profile(nrf_event& ev)
      : oai::sba::nf_profile(),
        m_event_sub(ev),
        hb_mutex(),
        first_update(true) {
    set_nf_type(NF_TYPE_UNKNOWN);
  }

  nrf_profile(nrf_event& ev, const nf_type_t type) : nrf_profile(ev) {
    set_nf_type(type);
  }

  nrf_profile(nrf_event& ev, const std::string& id)
      : oai::sba::nf_profile(id),
        m_event_sub(ev),
        hb_mutex(),
        first_update(true) {
    set_nf_type(NF_TYPE_UNKNOWN);
  }

  nrf_profile(nrf_profile& b) = delete;

  ~nrf_profile() override {
    Logger::nrf_app().debug("Delete NRF Profile instance...");
    if (hb_update_connection.connected()) hb_update_connection.disconnect();
    if (first_hb_connection.connected()) first_hb_connection.disconnect();
  }

  // The shared profile owns all generic NFProfile fields and operations.
  // NRF keeps only the enum/string adapter used by its matching logic.
  nf_type_t get_nf_type_enum() const;
  void set_nf_type(const nf_type_t& type);
  using oai::sba::nf_profile::set_nf_type;

  /*
   * Subscribe to the HBT timeout event (after receiving NF Update)
   * @param [uint64_t] ms: current time
   * @return void
   */
  void subscribe_heartbeat_timeout_nfupdate(uint64_t ms);

  /*
   * Subscribe to the HBT timeout event (after receiving NF Registration)
   * @param [uint64_t] ms: current time
   * @return void
   */
  void subscribe_heartbeat_timeout_nfregistration(uint64_t ms);

  /*
   * Handle heartbeat timeout event
   * @param [uint64_t] ms: current time
   * @return void
   */
  void handle_heartbeat_timeout(uint64_t ms);

  /*
   * Handle heartbeat timeout event after NF Registration
   * @param [uint64_t] ms: current time
   * @return void
   */
  void handle_heartbeat_timeout_nfregistration(uint64_t ms);

  /*
   * Handle heartbeat timeout event after NF Update
   * @param [uint64_t] ms: current time
   * @return void
   */
  void handle_heartbeat_timeout_nfupdate(uint64_t ms);

  /*
   * Unubscribe to HBT event (after NF Registration)
   * @param void
   * @return void
   */
  bool unsubscribe_heartbeat_timeout_nfregistration();

  /*
   * Unubscribe to HBT event (after NF Update)
   * @param void
   * @return void
   */
  bool unsubscribe_heartbeat_timeout_nfupdate();

  /*
   * Verify whether HBT is currently active
   * @param void
   * @return true if active, otherwise return false
   */
  bool is_heartbeat_timeout_nfupdate_active();

 protected:
  nrf_event& m_event_sub;
  bs2::connection
      hb_update_connection;  // connection for the HBT timeout (after NF Update)
  bs2::connection first_hb_connection;  // connection for first HBT timeout
                                        // (after NF Registration)
  mutable std::shared_mutex hb_mutex;
  bool first_update;
};

class amf_profile : public nrf_profile {
 public:
  amf_profile(nrf_event& ev) : nrf_profile(ev, NF_TYPE_AMF) { amf_info = {}; }

  amf_profile(nrf_event& ev, const std::string& id) : nrf_profile(ev, id) {
    set_nf_type(NF_TYPE_AMF);
    amf_info = {};
  }

  amf_profile(amf_profile& b) = delete;

  ~amf_profile() {}

  /*
   * Add an AMF info
   * @param [const amf_info_t &] info: AMF info
   * @return void
   */
  void add_amf_info(const amf_info_t& info);

  /*
   * Get list of AMF infos an AMF info
   * @param [const amf_info_t &] info: AMF info
   * @return void
   */
  void get_amf_info(amf_info_t& info) const;

  /*
   * Print related-information for an AMF profile
   * @param void
   * @return void:
   */
  void display();

  /*
   * Update a new value for a member of AMF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return void
   */
  bool replace_profile_info(const std::string& path, const std::string& value);

  /*
   * Add a new value for a member of NF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool add_profile_info(const std::string& path, const std::string& value);

  /*
   * Remove value of a member of NF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool remove_profile_info(const std::string& path);

  /*
   * Represent NF profile as json object
   * @param [nlohmann::json &] data: Json data
   * @return void
   */
  void to_json(nlohmann::json& data) const;

 private:
  amf_info_t amf_info;
};

class smf_profile : public nrf_profile {
 public:
  smf_profile(nrf_event& ev) : nrf_profile(ev, NF_TYPE_SMF) { smf_info = {}; }

  smf_profile(nrf_event& ev, const std::string& id) : nrf_profile(ev, id) {
    set_nf_type(NF_TYPE_SMF);
    smf_info = {};
  }

  smf_profile(smf_profile& b) = delete;

  /*
   * Add a SMF info
   * @param [const smf_info_t &] info: SMF info
   * @return void
   */
  void add_smf_info(const smf_info_t& info);

  /*
   * Get list of SMF infos a SMF info
   * @param [const smf_info_t &] info: SMF info
   * @return void
   */
  void get_smf_info(smf_info_t& infos) const;

  /*
   * Print related-information for a SMF profile
   * @param void
   * @return void:
   */
  void display();

  /*
   * Update a new value for a member of SMF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return void
   */
  bool replace_profile_info(const std::string& path, const std::string& value);

  /*
   * Add a new value for a member of NF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool add_profile_info(const std::string& path, const std::string& value);

  /*
   * Remove value of a member of NF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool remove_profile_info(const std::string& path);

  /*
   * Represent NF profile as json object
   * @param [nlohmann::json &] data: Json data
   * @return void
   */
  void to_json(nlohmann::json& data) const;

 private:
  smf_info_t smf_info;
};

class upf_profile : public nrf_profile {
 public:
  upf_profile(nrf_event& ev) : nrf_profile(ev, NF_TYPE_UPF) { upf_info = {}; }

  upf_profile(nrf_event& ev, const std::string& id) : nrf_profile(ev, id) {
    set_nf_type(NF_TYPE_UPF);
    upf_info = {};
  }

  upf_profile(upf_profile& b) = delete;

  /*
   * Add a UPF info
   * @param [const upf_info_t &] info: UPF info
   * @return void
   */
  void add_upf_info(const upf_info_t& info);

  /*
   * Get list of UPF infos a UPF info
   * @param [const upf_info_t &] info: UPF info
   * @return void
   */
  void get_upf_info(upf_info_t& infos) const;

  /*
   * Print related-information for a UPF profile
   * @param void
   * @return void:
   */
  void display();

  /*
   * Update a new value for a member of UPF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return void
   */
  bool replace_profile_info(const std::string& path, const std::string& value);

  /*
   * Add a new value for a member of NF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool add_profile_info(const std::string& path, const std::string& value);

  /*
   * Remove value of a member of NF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool remove_profile_info(const std::string& path);

  /*
   * Represent NF profile as json object
   * @param [nlohmann::json &] data: Json data
   * @return void
   */
  void to_json(nlohmann::json& data) const;

 private:
  upf_info_t upf_info;
};

class ausf_profile : public nrf_profile {
 public:
  ausf_profile(nrf_event& ev) : nrf_profile(ev, NF_TYPE_AUSF) {
    ausf_info = {};
  }

  ausf_profile(nrf_event& ev, const std::string& id) : nrf_profile(ev, id) {
    set_nf_type(NF_TYPE_AUSF);
    ausf_info = {};
  }

  ausf_profile(ausf_profile& b) = delete;

  /*
   * Add a AUSF info
   * @param [const ausf_info_t &] info: AUSF info
   * @return void
   */
  void add_ausf_info(const ausf_info_t& info);

  /*
   * Get list of AUSF infos a AUSF info
   * @param [const ausf_info_t &] info: AUSF info
   * @return void
   */
  void get_ausf_info(ausf_info_t& infos) const;

  /*
   * Print related-information for a AUSF profile
   * @param void
   * @return void:
   */
  void display();

  /*
   * Update a new value for a member of AUSF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return void
   */
  bool replace_profile_info(const std::string& path, const std::string& value);

  /*
   * Add a new value for a member of NF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool add_profile_info(const std::string& path, const std::string& value);

  /*
   * Remove value of a member of NF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool remove_profile_info(const std::string& path);

  /*
   * Represent NF profile as json object
   * @param [nlohmann::json &] data: Json data
   * @return void
   */
  void to_json(nlohmann::json& data) const;

 private:
  ausf_info_t ausf_info;
};

class udm_profile : public nrf_profile {
 public:
  udm_profile(nrf_event& ev) : nrf_profile(ev, NF_TYPE_UDM) { udm_info = {}; }

  udm_profile(nrf_event& ev, const std::string& id) : nrf_profile(ev, id) {
    set_nf_type(NF_TYPE_UDM);
    udm_info = {};
  }

  udm_profile(udm_profile& b) = delete;

  /*
   * Add a UDM info
   * @param [const udm_info_t &] info: UDM info
   * @return void
   */
  void add_udm_info(const udm_info_t& info);

  /*
   * Get list of UDM infos a UDM info
   * @param [const udm_info_t &] info: UDM info
   * @return void
   */
  void get_udm_info(udm_info_t& infos) const;

  /*
   * Print related-information for a UDM profile
   * @param void
   * @return void:
   */
  void display();

  /*
   * Update a new value for a member of UDM profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return void
   */
  bool replace_profile_info(const std::string& path, const std::string& value);

  /*
   * Add a new value for a member of UDM profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool add_profile_info(const std::string& path, const std::string& value);

  /*
   * Remove value of a member of UDM profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool remove_profile_info(const std::string& path);

  /*
   * Represent UDM profile as json object
   * @param [nlohmann::json &] data: Json data
   * @return void
   */
  void to_json(nlohmann::json& data) const;

 private:
  udm_info_t udm_info;
};

class udr_profile : public nrf_profile {
 public:
  udr_profile(nrf_event& ev) : nrf_profile(ev, NF_TYPE_UDR) { udr_info = {}; }

  udr_profile(nrf_event& ev, const std::string& id) : nrf_profile(ev, id) {
    set_nf_type(NF_TYPE_UDR);
    udr_info = {};
  }

  udr_profile(udr_profile& b) = delete;

  /*
   * Add a UDR info
   * @param [const udr_info_t &] info: UDR info
   * @return void
   */
  void add_udr_info(const udr_info_t& info);

  /*
   * Get list of UDR infos a UDR info
   * @param [const udr_info_t &] info: UDR info
   * @return void
   */
  void get_udr_info(udr_info_t& infos) const;

  /*
   * Print related-information for a UDR profile
   * @param void
   * @return void:
   */
  void display();

  /*
   * Update a new value for a member of UDR profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return void
   */
  bool replace_profile_info(const std::string& path, const std::string& value);

  /*
   * Add a new value for a member of UDR profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool add_profile_info(const std::string& path, const std::string& value);

  /*
   * Remove value of a member of UDR profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool remove_profile_info(const std::string& path);

  /*
   * Represent UDR profile as json object
   * @param [nlohmann::json &] data: Json data
   * @return void
   */
  void to_json(nlohmann::json& data) const;

 private:
  udr_info_t udr_info;
};

class pcf_profile : public nrf_profile {
 public:
  pcf_profile(nrf_event& ev) : nrf_profile(ev, NF_TYPE_PCF) { pcf_info = {}; }

  pcf_profile(nrf_event& ev, const std::string& id) : nrf_profile(ev, id) {
    set_nf_type(NF_TYPE_PCF);
    pcf_info = {};
  }

  pcf_profile(pcf_profile& b) = delete;

  /*
   * Add a PCF info
   * @param [const pcf_info_t &] info: PCF info
   * @return void
   */
  void add_pcf_info(const pcf_info_t& info);

  /*
   * Get list of PCF infos a PCF info
   * @param [const pcf_info_t &] info: pcf info
   * @return void
   */
  void get_pcf_info(pcf_info_t& infos) const;

  /*
   * Print related-information for a PCF profile
   * @param void
   * @return void:
   */
  void display();

  /*
   * Update a new value for a member of PCF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return void
   */
  bool replace_profile_info(const std::string& path, const std::string& value);

  /*
   * Add a new value for a member of PCF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool add_profile_info(const std::string& path, const std::string& value);

  /*
   * Remove value of a member of PCF profile
   * @param [const std::string &] path: member name
   * @param [const std::string &] value: new value
   * @return true if success, otherwise false
   */
  bool remove_profile_info(const std::string& path);

  /*
   * Represent PCF profile as json object
   * @param [nlohmann::json &] data: Json data
   * @return void
   */
  void to_json(nlohmann::json& data) const;

 private:
  pcf_info_t pcf_info;
};

}  // namespace app
}  // namespace nrf
}  // namespace oai

#endif
