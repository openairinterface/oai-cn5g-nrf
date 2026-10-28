# SPDX-License-Identifier: LicenseRef-CSSL-1.0

include_directories(${SRC_TOP_DIR}/../build/ext/cpp-jwt/include)
include_directories(${SRC_TOP_DIR}/api-server/api)
include_directories(${SRC_TOP_DIR}/api-server/impl)
include_directories(${SRC_TOP_DIR}/api-server/)
include_directories(${SRC_TOP_DIR}/common/)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/sba)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/utils)

include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/config/config.cmake)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model/nrf_model.cmake)

# NRF is the registry itself, so it uses the shared SBA profile, event and task
# manager but not nf_service (the latter is the NRF-registration client used by
# the other NFs and is coupled to the nghttp client).
set(NRF_COMMON_SBA_SRC_FILES
  ${SRC_TOP_DIR}/${MOUNTED_COMMON}/sba/nf_event.cpp
  ${SRC_TOP_DIR}/${MOUNTED_COMMON}/sba/nf_profile.cpp
  ${SRC_TOP_DIR}/${MOUNTED_COMMON}/sba/task_manager.cpp
)

add_library (NRF STATIC
  nrf_app.cpp
  nrf_config_types.cpp
  nrf_profile.cpp
  nrf_subscription.cpp 
  nrf_client.cpp 
  nrf_search_result.cpp 
  nrf_jwt.cpp 
  nrf_event.cpp
  ${NRF_COMMON_SBA_SRC_FILES}
)
