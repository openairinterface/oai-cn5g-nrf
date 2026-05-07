/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nrf_config_types.hpp"

#include <climits>

#include "logger.hpp"
#include "nrf_config.hpp"

using namespace oai::config::nrf;

//------------------------------------------------------------------------------
nrf_config_type::nrf_config_type(
    const std::string& name, const std::string& host, const sbi_interface& sbi)
    : nf(name, host, sbi) {
  m_config_name = "NRF Config";
  m_heartbeat   = int_config_value(
      NRF_CONFIG_HEARTBEAT_LABEL, 10);  // Default value: 10 seconds
  m_heartbeat.set_validation_interval(1, 65535);
  m_suspended_nf_interval = int_config_value(
      NRF_CONFIG_SUSPENDED_NF_INTERVAL_LABEL,
      300);  // Default value: 300 seconds (5 minutes)
  m_suspended_nf_interval.set_validation_interval(20, 65535);
  m_http2_worker_threads = int_config_value(NRF_CONFIG_HTTP2_WORKER_THREADS, 4);
  m_http2_worker_threads.set_validation_interval(0, 4096);
  m_http2_max_pending_tasks =
      int_config_value(NRF_CONFIG_HTTP2_MAX_PENDING_TASKS, 10000);
  m_http2_max_pending_tasks.set_validation_interval(0, INT32_MAX);
  m_http2_max_connections =
      int_config_value(NRF_CONFIG_HTTP2_MAX_CONNECTIONS, 10000);
  m_http2_max_connections.set_validation_interval(1, INT32_MAX);
  m_http2_max_concurrent_streams =
      int_config_value(NRF_CONFIG_HTTP2_MAX_CONCURRENT_STREAMS, 1000);
  m_http2_max_concurrent_streams.set_validation_interval(1, INT32_MAX);
  m_http2_initial_window_size =
      int_config_value(NRF_CONFIG_HTTP2_INITIAL_WINDOW_SIZE, 65535);
  m_http2_initial_window_size.set_validation_interval(1, INT32_MAX);
  m_http2_max_header_list_size =
      int_config_value(NRF_CONFIG_HTTP2_MAX_HEADER_LIST_SIZE, 65536);
  m_http2_max_header_list_size.set_validation_interval(1, INT32_MAX);
  m_http2_max_request_body_size =
      int_config_value(NRF_CONFIG_HTTP2_MAX_REQUEST_BODY_SIZE, 1024 * 1024);
  m_http2_max_request_body_size.set_validation_interval(1, INT32_MAX);
  m_http2_connection_idle_timeout_sec =
      int_config_value(NRF_CONFIG_HTTP2_IDLE_TIMEOUT_SEC, 60);
  m_http2_connection_idle_timeout_sec.set_validation_interval(1, INT32_MAX);
  m_http2_shutdown_drain_timeout_sec =
      int_config_value(NRF_CONFIG_HTTP2_SHUTDOWN_DRAIN_TIMEOUT_SEC, 5);
  m_http2_shutdown_drain_timeout_sec.set_validation_interval(0, INT32_MAX);
  m_http2_listener_backlog =
      int_config_value(NRF_CONFIG_HTTP2_LISTENER_BACKLOG, -1);
  m_http2_listener_backlog.set_validation_interval(-1, INT32_MAX);
}

//------------------------------------------------------------------------------
void nrf_config_type::from_yaml(const YAML::Node& node) {
  nf::from_yaml(node);

  // Load NRF specified parameter
  for (const auto& elem : node) {
    auto key = elem.first.as<std::string>();

    if (key == NRF_CONFIG_HEARTBEAT) {
      m_heartbeat.from_yaml(elem.second);
    }
    if (key == NRF_CONFIG_SUSPENDED_NF_INTERVAL) {
      m_suspended_nf_interval.from_yaml(elem.second);
    }
    if (key == NRF_CONFIG_HTTP2_SERVER) {
      for (const auto& http2_elem : elem.second) {
        auto http2_key = http2_elem.first.as<std::string>();
        if (http2_key == NRF_CONFIG_HTTP2_WORKER_THREADS) {
          m_http2_worker_threads.from_yaml(http2_elem.second);
        } else if (http2_key == NRF_CONFIG_HTTP2_MAX_PENDING_TASKS) {
          m_http2_max_pending_tasks.from_yaml(http2_elem.second);
        } else if (http2_key == NRF_CONFIG_HTTP2_MAX_CONNECTIONS) {
          m_http2_max_connections.from_yaml(http2_elem.second);
        } else if (http2_key == NRF_CONFIG_HTTP2_MAX_CONCURRENT_STREAMS) {
          m_http2_max_concurrent_streams.from_yaml(http2_elem.second);
        } else if (http2_key == NRF_CONFIG_HTTP2_INITIAL_WINDOW_SIZE) {
          m_http2_initial_window_size.from_yaml(http2_elem.second);
        } else if (http2_key == NRF_CONFIG_HTTP2_MAX_HEADER_LIST_SIZE) {
          m_http2_max_header_list_size.from_yaml(http2_elem.second);
        } else if (http2_key == NRF_CONFIG_HTTP2_MAX_REQUEST_BODY_SIZE) {
          m_http2_max_request_body_size.from_yaml(http2_elem.second);
        } else if (http2_key == NRF_CONFIG_HTTP2_IDLE_TIMEOUT_SEC) {
          m_http2_connection_idle_timeout_sec.from_yaml(http2_elem.second);
        } else if (http2_key == NRF_CONFIG_HTTP2_SHUTDOWN_DRAIN_TIMEOUT_SEC) {
          m_http2_shutdown_drain_timeout_sec.from_yaml(http2_elem.second);
        } else if (http2_key == NRF_CONFIG_HTTP2_LISTENER_BACKLOG) {
          m_http2_listener_backlog.from_yaml(http2_elem.second);
        }
      }
    }
  }
}

//------------------------------------------------------------------------------
nlohmann::json nrf_config_type::to_json() {
  nlohmann::json json_data                 = {};
  json_data                                = nf::to_json();
  json_data[m_heartbeat.get_config_name()] = m_heartbeat.to_json();
  json_data[m_suspended_nf_interval.get_config_name()] =
      m_suspended_nf_interval.to_json();
  json_data[NRF_CONFIG_HTTP2_SERVER] = {
      {m_http2_worker_threads.get_config_name(),
       m_http2_worker_threads.to_json()},
      {m_http2_max_pending_tasks.get_config_name(),
       m_http2_max_pending_tasks.to_json()},
      {m_http2_max_connections.get_config_name(),
       m_http2_max_connections.to_json()},
      {m_http2_max_concurrent_streams.get_config_name(),
       m_http2_max_concurrent_streams.to_json()},
      {m_http2_initial_window_size.get_config_name(),
       m_http2_initial_window_size.to_json()},
      {m_http2_max_header_list_size.get_config_name(),
       m_http2_max_header_list_size.to_json()},
      {m_http2_max_request_body_size.get_config_name(),
       m_http2_max_request_body_size.to_json()},
      {m_http2_connection_idle_timeout_sec.get_config_name(),
       m_http2_connection_idle_timeout_sec.to_json()},
      {m_http2_shutdown_drain_timeout_sec.get_config_name(),
       m_http2_shutdown_drain_timeout_sec.to_json()},
      {m_http2_listener_backlog.get_config_name(),
       m_http2_listener_backlog.to_json()}};
  return json_data;
}

//------------------------------------------------------------------------------
bool nrf_config_type::from_json(const nlohmann::json& json_data) {
  try {
    nf::from_json(json_data);
    if (json_data.find(m_heartbeat.get_config_name()) != json_data.end()) {
      m_heartbeat.from_json(json_data[m_heartbeat.get_config_name()]);
    }
    if (json_data.find(m_suspended_nf_interval.get_config_name()) !=
        json_data.end()) {
      m_suspended_nf_interval.from_json(
          json_data[m_suspended_nf_interval.get_config_name()]);
    }
    if (json_data.find(NRF_CONFIG_HTTP2_SERVER) != json_data.end()) {
      const auto& http2_json = json_data[NRF_CONFIG_HTTP2_SERVER];
      if (http2_json.find(m_http2_worker_threads.get_config_name()) !=
          http2_json.end()) {
        m_http2_worker_threads.from_json(
            http2_json[m_http2_worker_threads.get_config_name()]);
      }
      if (http2_json.find(m_http2_max_pending_tasks.get_config_name()) !=
          http2_json.end()) {
        m_http2_max_pending_tasks.from_json(
            http2_json[m_http2_max_pending_tasks.get_config_name()]);
      }
      if (http2_json.find(m_http2_max_connections.get_config_name()) !=
          http2_json.end()) {
        m_http2_max_connections.from_json(
            http2_json[m_http2_max_connections.get_config_name()]);
      }
      if (http2_json.find(m_http2_max_concurrent_streams.get_config_name()) !=
          http2_json.end()) {
        m_http2_max_concurrent_streams.from_json(
            http2_json[m_http2_max_concurrent_streams.get_config_name()]);
      }
      if (http2_json.find(m_http2_initial_window_size.get_config_name()) !=
          http2_json.end()) {
        m_http2_initial_window_size.from_json(
            http2_json[m_http2_initial_window_size.get_config_name()]);
      }
      if (http2_json.find(m_http2_max_header_list_size.get_config_name()) !=
          http2_json.end()) {
        m_http2_max_header_list_size.from_json(
            http2_json[m_http2_max_header_list_size.get_config_name()]);
      }
      if (http2_json.find(m_http2_max_request_body_size.get_config_name()) !=
          http2_json.end()) {
        m_http2_max_request_body_size.from_json(
            http2_json[m_http2_max_request_body_size.get_config_name()]);
      }
      if (http2_json.find(
              m_http2_connection_idle_timeout_sec.get_config_name()) !=
          http2_json.end()) {
        m_http2_connection_idle_timeout_sec.from_json(
            http2_json[m_http2_connection_idle_timeout_sec.get_config_name()]);
      }
      if (http2_json.find(
              m_http2_shutdown_drain_timeout_sec.get_config_name()) !=
          http2_json.end()) {
        m_http2_shutdown_drain_timeout_sec.from_json(
            http2_json[m_http2_shutdown_drain_timeout_sec.get_config_name()]);
      }
      if (http2_json.find(m_http2_listener_backlog.get_config_name()) !=
          http2_json.end()) {
        m_http2_listener_backlog.from_json(
            http2_json[m_http2_listener_backlog.get_config_name()]);
      }
    }
    return true;
  } catch (nlohmann::detail::exception& e) {
    // TODO:
  } catch (std::exception& e) {
    // TODO:
  }
  return false;
}

//------------------------------------------------------------------------------
std::string nrf_config_type::to_string(const std::string& indent) const {
  std::string out          = {};
  std::string inner_indent = indent + indent;
  unsigned int inner_width = get_inner_width(inner_indent.length());
  out.append(nf::to_string(indent));

  out.append(inner_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM, m_heartbeat.get_config_name(),
          inner_width, std::to_string(m_heartbeat.get_value()) + " (seconds)"));

  out.append(inner_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_suspended_nf_interval.get_config_name(), inner_width,
          std::to_string(m_suspended_nf_interval.get_value()) + " (seconds)"));

  out.append(inner_indent).append(NRF_CONFIG_HTTP2_SERVER_LABEL).append(":\n");
  auto http2_indent        = inner_indent + inner_indent;
  unsigned int http2_width = get_inner_width(http2_indent.length());
  out.append(http2_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_http2_worker_threads.get_config_name(), http2_width,
          m_http2_worker_threads.get_value()));
  out.append(http2_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_http2_max_pending_tasks.get_config_name(), http2_width,
          m_http2_max_pending_tasks.get_value()));
  out.append(http2_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_http2_max_connections.get_config_name(), http2_width,
          m_http2_max_connections.get_value()));
  out.append(http2_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_http2_max_concurrent_streams.get_config_name(), http2_width,
          m_http2_max_concurrent_streams.get_value()));
  out.append(http2_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_http2_initial_window_size.get_config_name(), http2_width,
          m_http2_initial_window_size.get_value()));
  out.append(http2_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_http2_max_header_list_size.get_config_name(), http2_width,
          m_http2_max_header_list_size.get_value()));
  out.append(http2_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_http2_max_request_body_size.get_config_name(), http2_width,
          m_http2_max_request_body_size.get_value()));
  out.append(http2_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_http2_connection_idle_timeout_sec.get_config_name(), http2_width,
          m_http2_connection_idle_timeout_sec.get_value()));
  out.append(http2_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_http2_shutdown_drain_timeout_sec.get_config_name(), http2_width,
          m_http2_shutdown_drain_timeout_sec.get_value()));
  out.append(http2_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM,
          m_http2_listener_backlog.get_config_name(), http2_width,
          m_http2_listener_backlog.get_value()));

  return out;
}

//------------------------------------------------------------------------------
void nrf_config_type::validate() {
  nf::validate();
  m_heartbeat.validate();
  m_suspended_nf_interval.validate();
  m_http2_worker_threads.validate();
  m_http2_max_pending_tasks.validate();
  m_http2_max_connections.validate();
  m_http2_max_concurrent_streams.validate();
  m_http2_initial_window_size.validate();
  m_http2_max_header_list_size.validate();
  m_http2_max_request_body_size.validate();
  m_http2_connection_idle_timeout_sec.validate();
  m_http2_shutdown_drain_timeout_sec.validate();
  m_http2_listener_backlog.validate();
}

//------------------------------------------------------------------------------
uint16_t nrf_config_type::get_heartbeat() const {
  return m_heartbeat.get_value();
}

//------------------------------------------------------------------------------
uint16_t nrf_config_type::get_suspended_nf_interval() const {
  return m_suspended_nf_interval.get_value();
}

//------------------------------------------------------------------------------
uint32_t nrf_config_type::get_http2_worker_threads() const {
  return static_cast<uint32_t>(m_http2_worker_threads.get_value());
}

//------------------------------------------------------------------------------
size_t nrf_config_type::get_http2_max_pending_tasks() const {
  return static_cast<size_t>(m_http2_max_pending_tasks.get_value());
}

//------------------------------------------------------------------------------
uint32_t nrf_config_type::get_http2_max_connections() const {
  return static_cast<uint32_t>(m_http2_max_connections.get_value());
}

//------------------------------------------------------------------------------
uint32_t nrf_config_type::get_http2_max_concurrent_streams() const {
  return static_cast<uint32_t>(m_http2_max_concurrent_streams.get_value());
}

//------------------------------------------------------------------------------
uint32_t nrf_config_type::get_http2_initial_window_size() const {
  return static_cast<uint32_t>(m_http2_initial_window_size.get_value());
}

//------------------------------------------------------------------------------
uint32_t nrf_config_type::get_http2_max_header_list_size() const {
  return static_cast<uint32_t>(m_http2_max_header_list_size.get_value());
}

//------------------------------------------------------------------------------
size_t nrf_config_type::get_http2_max_request_body_size() const {
  return static_cast<size_t>(m_http2_max_request_body_size.get_value());
}

//------------------------------------------------------------------------------
int nrf_config_type::get_http2_connection_idle_timeout_sec() const {
  return m_http2_connection_idle_timeout_sec.get_value();
}

//------------------------------------------------------------------------------
int nrf_config_type::get_http2_shutdown_drain_timeout_sec() const {
  return m_http2_shutdown_drain_timeout_sec.get_value();
}

//------------------------------------------------------------------------------
int nrf_config_type::get_http2_listener_backlog() const {
  return m_http2_listener_backlog.get_value();
}
