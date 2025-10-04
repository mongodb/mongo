#!/bin/sh

# Usage:
# XCODE_VERSION = Dotted version of Xcode to install and make active.
# XCODE_INSTALL_USER = Account name for developer.apple.com without TFA.
# XCODE_INSTALL_PASSWORD = Password for that account.

set -e
sudo gem install xcode-install
xcversion update
echo ">>>>>"
echo ">>>>> AVAILABLE.."
echo ">>>>>"
xcversion list
echo ">>>>>"
echo ">>>>> INSTALLL: Xcode ${XCODE_VERSION}.."
echo ">>>>>"
xcversion --verbose --no-progress --no-clean --no-show-release-notes install ${XCODE_VERSION}
echo ""
echo ">>>>>"
echo ">>>>> INSTALL: CLI Tools.."
echo ">>>>>"
xcversion --verbose install-cli-tools
echo ""
echo ">>>>>"
echo ">>>>> AVAILABLE.."
echo ">>>>>"
xcversion list
echo ">>>>>"
echo ">>>>> SELECT: Xcode ${XCODE_VERSION}.."
echo ">>>>>"
xcversion select ${XCODE_VERSION} --symlink

# Use, modification, and distribution are
# subject to the Boost Software License, Version 1.0. (See accompanying
# file LICENSE.txt)
#
# Copyright Rene Rivera 2020.
