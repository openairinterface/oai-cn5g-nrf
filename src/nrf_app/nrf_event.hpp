/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NRF_EVENT_HPP_SEEN
#define FILE_NRF_EVENT_HPP_SEEN

#include <boost/signals2.hpp>
namespace bs2 = boost::signals2;

#include "nf_event.hpp"
#include "nrf.h"
#include "nrf_event_sig.hpp"

namespace oai {
namespace nrf {
namespace app {

// class nrf_profile;

class nrf_event : public oai::sba::nf_event {
 public:
  nrf_event()                      = default;
  nrf_event(nrf_event const&)      = delete;
  void operator=(nrf_event const&) = delete;

  static nrf_event& get_instance() {
    static nrf_event instance;
    return instance;
  }

  // class register/handle event
  friend class nrf_app;
  friend class nrf_profile;

  /*
   * Subscribe to the nf status change event
   * @param [const nf_status_change_sig_t::slot_type &] sig
   * @return boost::signals2::connection
   */
  bs2::connection subscribe_nf_status_change(
      const nf_status_change_sig_t::slot_type& sig);

  bs2::connection subscribe_nf_status_registered(
      const nf_status_sig_t::slot_type& sig);

  bs2::connection subscribe_nf_status_deregistered(
      const nf_deregistered_sig_t::slot_type& sig);

  bs2::connection subscribe_nf_status_profile_changed(
      const nf_status_sig_t::slot_type& sig);

 private:
  nf_status_change_sig_t nf_status_change;
  nf_status_sig_t nf_status_registered;
  nf_deregistered_sig_t nf_status_deregistered;
  nf_status_sig_t nf_status_profile_changed;
};
}  // namespace app
}  // namespace nrf
}  // namespace oai

#endif /* FILE_NRF_EVENT_HPP_SEEN */
