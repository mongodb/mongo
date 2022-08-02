#!/bin/bash

# This script is loaded on the target machine, which is running tests
# Purpose: install mongod and shell from packages

set -o xtrace
set -o errexit

function apply_selinux_policy() {
  echo "==== Applying SELinux policy now"
  rm -rf mongodb-selinux
  git clone https://github.com/mongodb/mongodb-selinux
  cd mongodb-selinux
  make
  sudo make install
}

# on evergreen images /tmp is usually linked to /data/tmp, which interferes
# with selinux, as it does not recognize it as tmp_t domain
if [ -L /tmp ]; then
  sudo --non-interactive rm /tmp
  sudo --non-interactive mkdir /tmp
  sudo --non-interactive systemctl start tmp.mount
fi

# selinux policy should work both when applied before and after install
# we will randomly apply it before or after installation is completed
SEORDER="$(($RANDOM % 2))"
if [ "$SEORDER" == "0" ]; then
  apply_selinux_policy
fi

pkg="$(find "$HOME"/repo -name 'mongodb-*-server-*.x86_64.rpm' | tee /dev/stderr)"
if ! sudo --non-interactive rpm --install --verbose --verbose --hash --nodeps "$pkg"; then
  if [ "$?" -gt "1" ]; then exit 1; fi # exit code 1 is OK
fi

if [ "$SEORDER" == "1" ]; then
  apply_selinux_policy
fi
