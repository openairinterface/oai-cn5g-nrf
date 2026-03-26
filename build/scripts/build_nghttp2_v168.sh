#!/bin/bash
################################################################################
# Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The OpenAirInterface Software Alliance licenses this file to You under
# the OAI Public License, Version 1.1  (the "License"); you may not use this file
# except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.openairinterface.org/?page_id=698
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#-------------------------------------------------------------------------------
# For more information about the OpenAirInterface (OAI) Software Alliance:
#      contact@openairinterface.org
################################################################################

# NRF-specific nghttp2 v1.68.0 build script.
# Builds the nghttp2 C library ONLY (no asio wrapper).
# This replaces the shared common-build install_nghttp2_from_git() for
# the NRF, which only links -lnghttp2 (C API).
#
# The shared common-build submodule (build_helper.nghttp2) is NOT modified.
# Other NFs continue to use v1.43.0-DEV with --enable-asio-lib.
#
# EXACT VERSION PIN: v1.68.0 — NGHTTP2_VERSION_NUM == 0x014400
# Do NOT change to v1.68.1 or any newer version without plan approval.

NGHTTP2_VERSION="v1.68.0"
# Fallback: pinned commit hash for v1.68.0, in case the tag is ever
# repointed or temporarily unavailable.
NGHTTP2_COMMIT="6b9e5ad6a0067562198f28e3fae11efc9da15f24"
NGHTTP2_GIT_URL="https://github.com/nghttp2/nghttp2.git"

#-------------------------------------------------------------------------------
# arg1 is force (0 or 1) (no interactive script)
# arg2 is debug (0 or 1) (install debug libraries)
install_nghttp2_v168_from_git() {
  # Preserve CI-compatible log strings.
  # ci-scripts/common/python/building_report.py parses these exact strings.
  echo "Starting to install nghttp2"

  if [ $1 -eq 0 ]; then
    read -p "Do you want to install nghttp2 v1.68.0? <y/N> " prompt
    OPTION=""
  else
    prompt='y'
    OPTION="-y"
  fi

  if [ $2 -eq 0 ]; then
    debug=0
  else
    debug=1
  fi

  if [[ $prompt =~ [yY](es)* ]]; then
    # Install build dependencies (CMake is already in PACKAGE_LIST from
    # check_install_nrf_deps; these are the nghttp2-specific ones).
    if [[ $OS_DISTRO == "ubuntu" ]]; then
      PACKAGE_LIST="\
        g++ \
        $CMAKE \
        binutils \
        pkg-config \
        zlib1g-dev \
        libssl-dev \
        libevent-dev \
        libc-ares-dev"
    elif [[ "$OS_BASEDISTRO" == "fedora" ]]; then
      PACKAGE_LIST="\
        gcc-c++ \
        binutils-devel \
        $CMAKE \
        make \
        pkg-config \
        zlib-devel \
        openssl-devel \
        libevent-devel \
        c-ares-devel"
    else
      echo_fatal "$OS_DISTRO is not a supported distribution."
    fi
    echo "Install nghttp2 v1.68.0 build dependencies"
    $SUDO $INSTALLER install $OPTION $PACKAGE_LIST
    ret=$?;[[ $ret -ne 0 ]] && return $ret

    echo "Install nghttp2 v1.68.0 (C library only) from $NGHTTP2_GIT_URL"
    NGHTTP2_BUILD_DIR=$(mktemp -d)
    trap 'rm -rf "$NGHTTP2_BUILD_DIR"' EXIT
    pushd "$NGHTTP2_BUILD_DIR"

    git clone --depth 1 --branch "$NGHTTP2_VERSION" "$NGHTTP2_GIT_URL" nghttp2_v168
    ret=$?;[[ $ret -ne 0 ]] && popd && return $ret

    cd nghttp2_v168

    # Build with CMake — C library only, no app/examples/hpack tools.
    cmake -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_LIB_ONLY=ON \
      -DBUILD_TESTING=OFF \
      -DBUILD_SHARED_LIBS=ON \
      -DBUILD_STATIC_LIBS=OFF \
      -DCMAKE_INSTALL_LIBDIR=lib
    ret=$?;[[ $ret -ne 0 ]] && popd && return $ret

    cmake --build build -j "$(nproc)"
    ret=$?;[[ $ret -ne 0 ]] && popd && return $ret

    $SUDO cmake --install build
    ret=$?;[[ $ret -ne 0 ]] && popd && return $ret

    $SUDO ldconfig
    ret=$?;[[ $ret -ne 0 ]] && popd && return $ret

    # Cleanup is handled by the EXIT trap
    popd
  fi

  echo "nghttp2 installation complete"
  return 0
}

# ---------------------------------------------------------------------------
# Standalone execution entry point.
# When this script is run directly (not sourced), perform the install.
# ---------------------------------------------------------------------------
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  # Detect sudo availability
  if command -v sudo &>/dev/null; then
    SUDO="sudo"
  else
    SUDO=""
  fi

  # Detect OS distro
  if [[ -f /etc/os-release ]]; then
    . /etc/os-release
    OS_DISTRO="${ID}"
    OS_BASEDISTRO="${ID_LIKE:-$ID}"
  else
    OS_DISTRO="ubuntu"
    OS_BASEDISTRO="debian"
  fi

  # Detect package installer and cmake executable
  if command -v apt-get &>/dev/null; then
    INSTALLER="apt-get"
    CMAKE="cmake"
  elif command -v dnf &>/dev/null; then
    INSTALLER="dnf"
    CMAKE="cmake"
  elif command -v yum &>/dev/null; then
    INSTALLER="yum"
    CMAKE="cmake3"
  else
    echo "ERROR: No supported package manager found." >&2
    exit 1
  fi

  # force=1 (non-interactive), debug=0
  install_nghttp2_v168_from_git 1 0
  exit $?
fi
