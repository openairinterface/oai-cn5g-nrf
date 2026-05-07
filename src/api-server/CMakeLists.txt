# SPDX-License-Identifier: LicenseRef-CSSL-1.0

set(NRF_API_SERVER_DIR "${SRC_TOP_DIR}/api-server")

include_directories(${NRF_API_SERVER_DIR}/api)
include_directories(${NRF_API_SERVER_DIR}/impl)
include_directories(${NRF_API_SERVER_DIR}/)
include_directories(${SRC_TOP_DIR}/nrf_app)
include_directories(${SRC_TOP_DIR}/common)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/3gpp)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/common)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/utils)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/utils/bstr)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/config/config.cmake)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model/nrf_model.cmake)

file(GLOB NRF_API_SERVER_src_files
    ${NRF_API_SERVER_DIR}/http2-server.cpp
    ${NRF_API_SERVER_DIR}/nrf-api-server.cpp
    ${NRF_API_SERVER_DIR}/nrf-http2-server.cpp
    ${NRF_API_SERVER_DIR}/api/*.cpp
    ${NRF_API_SERVER_DIR}/impl/*.cpp
)
 
add_library(NRF_API STATIC
           ${NRF_API_SERVER_src_files})

if(BUILD_TESTING)
  add_executable(http2_server_smoke_test
    ${NRF_API_SERVER_DIR}/http2-server.cpp
    ${SRC_TOP_DIR}/../test/http2_server_smoke.cpp
    ${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger/nf-tp.cpp
    ${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger/lttng_logger_base.cpp
    ${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger/spd_logger_base.cpp
    ${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger/logger_base.cpp
  )
  target_link_libraries(http2_server_smoke_test
    -Wl,--start-group
    -lnghttp2
    -lssl
    -lcrypto
    -lfmt
    -lspdlog
    -Wl,--end-group
    event
    event_pthreads
    event_openssl
    curl
    pthread
    m
    rt
  )
  add_test(NAME http2_server_smoke_test COMMAND http2_server_smoke_test)
endif()
