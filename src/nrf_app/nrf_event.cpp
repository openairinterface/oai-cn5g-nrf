/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nrf_event.hpp"

using namespace oai::nrf::app;

//------------------------------------------------------------------------------
bs2::connection nrf_event::subscribe_nf_status_change(
    const nf_status_change_sig_t::slot_type& sig) {
  return nf_status_change.connect(sig);
}

//------------------------------------------------------------------------------
bs2::connection nrf_event::subscribe_nf_status_registered(
    const nf_status_sig_t::slot_type& sig) {
  return nf_status_registered.connect(sig);
}

//------------------------------------------------------------------------------
bs2::connection nrf_event::subscribe_nf_status_deregistered(
    const nf_deregistered_sig_t::slot_type& sig) {
  return nf_status_deregistered.connect(sig);
}

//------------------------------------------------------------------------------
bs2::connection nrf_event::subscribe_nf_status_profile_changed(
    const nf_status_sig_t::slot_type& sig) {
  return nf_status_profile_changed.connect(sig);
}
