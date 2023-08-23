#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_macos_cross
export CI_IMAGE_NAME_TAG="docker.io/ubuntu:22.04"
export HOST=x86_64-apple-darwin
export PACKAGES="cmake libz-dev libtinfo5 python3-setuptools xorriso"
export XCODE_VERSION=12.2
export XCODE_BUILD_ID=12B45b
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export GOAL="deploy"

# False-positive warning is fixed with clang 17, remove this when that version
# can be used.
export BITCOIN_CONFIG="--with-gui --enable-reduce-exports LDFLAGS=-Wno-error=unused-command-line-argument"
