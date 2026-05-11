/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <cstddef>

#include "config.hpp"

constexpr auto NRF_CONFIG_HEARTBEAT                   = "heartbeat";
constexpr auto NRF_CONFIG_HEARTBEAT_LABEL             = "Heartbeat";
constexpr auto NRF_CONFIG_SUSPENDED_NF_INTERVAL       = "suspended_nf_interval";
constexpr auto NRF_CONFIG_SUSPENDED_NF_INTERVAL_LABEL = "Suspended NF Interval";
constexpr auto NRF_CONFIG_HTTP2_SERVER                = "http2_server";
constexpr auto NRF_CONFIG_HTTP2_SERVER_LABEL          = "HTTP/2 Server";
constexpr auto NRF_CONFIG_HTTP2_WORKER_THREADS        = "worker_threads";
constexpr auto NRF_CONFIG_HTTP2_MAX_PENDING_TASKS     = "max_pending_tasks";
constexpr auto NRF_CONFIG_HTTP2_MAX_CONNECTIONS       = "max_connections";
constexpr auto NRF_CONFIG_HTTP2_MAX_CONCURRENT_STREAMS =
    "max_concurrent_streams";
constexpr auto NRF_CONFIG_HTTP2_INITIAL_WINDOW_SIZE   = "initial_window_size";
constexpr auto NRF_CONFIG_HTTP2_MAX_HEADER_LIST_SIZE  = "max_header_list_size";
constexpr auto NRF_CONFIG_HTTP2_MAX_REQUEST_BODY_SIZE = "max_request_body_size";
constexpr auto NRF_CONFIG_HTTP2_IDLE_TIMEOUT_SEC =
    "connection_idle_timeout_sec";
constexpr auto NRF_CONFIG_HTTP2_SHUTDOWN_DRAIN_TIMEOUT_SEC =
    "shutdown_drain_timeout_sec";
constexpr auto NRF_CONFIG_HTTP2_LISTENER_BACKLOG  = "listener_backlog";
constexpr auto NRF_CONFIG_HTTP2_ENABLE_MTLS       = "enable_mtls";
constexpr auto NRF_CONFIG_HTTP2_ENABLE_MTLS_LABEL = "Enable mTLS";
constexpr auto NRF_CONFIG_HTTP2_STATS_LOG_INTERVAL_SEC =
    "stats_log_interval_sec";

namespace oai::config::nrf {

class nrf_config_type : public oai::config::nf {
  friend class nrf_config;

 private:
  int_config_value m_heartbeat;
  int_config_value m_suspended_nf_interval;
  int_config_value m_http2_worker_threads;
  int_config_value m_http2_max_pending_tasks;
  int_config_value m_http2_max_connections;
  int_config_value m_http2_max_concurrent_streams;
  int_config_value m_http2_initial_window_size;
  int_config_value m_http2_max_header_list_size;
  int_config_value m_http2_max_request_body_size;
  int_config_value m_http2_connection_idle_timeout_sec;
  int_config_value m_http2_shutdown_drain_timeout_sec;
  int_config_value m_http2_listener_backlog;
  option_config_value m_http2_enable_mtls;
  int_config_value m_http2_stats_log_interval_sec;

 public:
  explicit nrf_config_type(
      const std::string& name, const std::string& host,
      const sbi_interface& sbi);
  void from_yaml(const YAML::Node& node) override;
  nlohmann::json to_json() override;
  bool from_json(const nlohmann::json& json_data) override;

  [[nodiscard]] std::string to_string(const std::string& indent) const override;

  void validate() override;

  [[nodiscard]] uint16_t get_heartbeat() const;
  void set_heartbeat(uint16_t);

  [[nodiscard]] uint16_t get_suspended_nf_interval() const;
  void set_suspended_nf_interval(uint16_t);
  [[nodiscard]] uint32_t get_http2_worker_threads() const;
  [[nodiscard]] size_t get_http2_max_pending_tasks() const;
  [[nodiscard]] uint32_t get_http2_max_connections() const;
  [[nodiscard]] uint32_t get_http2_max_concurrent_streams() const;
  [[nodiscard]] uint32_t get_http2_initial_window_size() const;
  [[nodiscard]] uint32_t get_http2_max_header_list_size() const;
  [[nodiscard]] size_t get_http2_max_request_body_size() const;
  [[nodiscard]] int get_http2_connection_idle_timeout_sec() const;
  [[nodiscard]] int get_http2_shutdown_drain_timeout_sec() const;
  [[nodiscard]] int get_http2_listener_backlog() const;
  [[nodiscard]] bool get_http2_enable_mtls() const;
  [[nodiscard]] uint32_t get_http2_stats_log_interval_sec() const;
};

}  // namespace oai::config::nrf
