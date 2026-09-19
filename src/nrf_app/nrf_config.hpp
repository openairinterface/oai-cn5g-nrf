/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include "config.hpp"
#include "logger.hpp"
#include <map>
#include <regex>
#include "nrf_config_types.hpp"

namespace oai::config::nrf {

class nrf_config : public oai::config::config {
  std::string m_roaming_path;

 public:
  bool roaming_enabled = false;
  std::vector<std::pair<std::string, std::string>> local_plmns;
  std::map<std::pair<std::string, std::string>, std::string> roaming_nrf_roots;
  std::string local_sepp_root;

  bool init() override {
    try {
      const auto root   = YAML::LoadFile(m_roaming_path);
      const auto policy = root["enable_roaming"];
      roaming_enabled =
          policy && policy["general"] && policy["general"].as<bool>();
      auto plmn = [](const YAML::Node& entry) {
        const auto mcc = entry["mcc"].as<std::string>();
        const auto mnc = entry["mnc"].as<std::string>();
        if (!std::regex_match(mcc, std::regex("[0-9]{3}")) ||
            !std::regex_match(mnc, std::regex("[0-9]{2,3}")))
          throw std::invalid_argument("Invalid roaming PLMN");
        return std::make_pair(mcc, mnc);
      };
      local_plmns.clear();
      roaming_nrf_roots.clear();
      const auto local_config = root["nrf"];
      const auto locals       = local_config ? local_config["plmn_list"] :
                                               YAML::Node(YAML::NodeType::Undefined);
      if (locals) {
        if (!locals.IsSequence())
          throw std::invalid_argument("nrf.plmn_list must be a sequence");
        for (const auto& entry : locals) local_plmns.push_back(plmn(entry));
      }
      if (policy && policy["roaming_partners"]) {
        const auto partners = policy["roaming_partners"];
        if (!partners.IsSequence())
          throw std::invalid_argument("roaming_partners must be a sequence");
        for (const auto& entry : partners) {
          auto api_root = entry["nrf_api_root"].as<std::string>("");
          while (!api_root.empty() && api_root.back() == '/')
            api_root.pop_back();
          if (!api_root.empty() &&
              !std::regex_match(
                  api_root, std::regex("https?://[A-Za-z0-9.-]+(:[0-9]+)?(/"
                                       "[A-Za-z0-9._~/-]+)?")))
            throw std::invalid_argument("Invalid partner nrf_api_root");
          if (!roaming_nrf_roots.emplace(plmn(entry), api_root).second)
            throw std::invalid_argument("Duplicate roaming partner PLMN");
        }
      }
      local_sepp_root.clear();
      const auto nfs = root["nfs"];
      const auto sepp =
          nfs ? nfs["sepp"] : YAML::Node(YAML::NodeType::Undefined);
      if (sepp) {
        const auto host   = sepp["host"].as<std::string>();
        const auto port   = sepp["sbi"]["port"].as<unsigned>();
        const auto scheme = sepp["sbi"]["scheme"].as<std::string>("http");
        if (!std::regex_match(host, std::regex("[A-Za-z0-9.-]+")) ||
            port == 0 || port > 65535 ||
            (scheme != "http" && scheme != "https"))
          throw std::invalid_argument("Invalid local SEPP SBI endpoint");
        local_sepp_root = scheme + "://" + host + ":" + std::to_string(port);
      }
      return config::init();
    } catch (const std::exception& e) {
      Logger::nrf_app().error(
          "Invalid NRF roaming configuration: %s", e.what());
      return false;
    }
  }
  // Stefan: we should get rid of this instance things (see PCF)
  unsigned int instance = 0;
  explicit nrf_config(
      const std::string& config_path, bool log_stdout, bool log_rot_file)
      : config(config_path, NRF_CONFIG_NAME, log_stdout, log_rot_file),
        m_roaming_path(config_path) {
    m_used_config_values = {
        LOG_LEVEL_CONFIG_NAME, NF_LIST_CONFIG_NAME, NF_CONFIG_HTTP_NAME,
        NRF_CONFIG_NAME};
    m_used_sbi_values = {NRF_CONFIG_NAME};

    m_register_nrf_feature.unset_config();

    auto nrf = std::make_shared<nrf_config_type>(
        NRF_CONFIG_NAME, "oai-nrf",
        sbi_interface("SBI", "oai-nrf", 80, "v1", "eth0"));
    add_nf(NRF_CONFIG_NAME, nrf);
  };

  std::shared_ptr<nrf_config_type> nrf() const {
    return std::static_pointer_cast<nrf_config_type>(get_local());
  };
};
}  // namespace oai::config::nrf
