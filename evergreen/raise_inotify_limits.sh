#!/usr/bin/env bash
# Raise inotify limits so many concurrent mongod/mongos processes (e.g. sharded
# resmoke suites) don't exhaust the default per-user watch/instance budget.
set -o errexit
set -o verbose

sudo sysctl -w fs.inotify.max_user_instances=8192
sudo sysctl -w fs.inotify.max_user_watches=524288
sudo sysctl -w fs.inotify.max_queued_events=65536
# If we have too many jobs we can creep into the ephemeral port range
sudo sysctl -w net.ipv4.ip_local_port_range="49152 60999"
