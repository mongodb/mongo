#!/usr/bin/bash
set -eo
TAG=$1

if [ -n "${TAG}" ]; then
  git checkout "${TAG}"
else
  git checkout eloqdoc-4.0.3
  git pull origin eloqdoc-4.0.3
fi
git submodule update --recursive

if [ -n "${TAG}" ]; then
  RAFT_HOST_MGR_HASH=$(awk -F'=' '{ if ($1 == "raft_host_manager") {print $2} }' .private_modules)
  LOG_SERVICE_HASH=$(awk -F'=' '{ if ($1 == "log_service") {print $2} }' .private_modules)
  pushd src/mongo/db/modules/eloq/tx_service/raft_host_manager
  git checkout ${RAFT_HOST_MGR_HASH}
  popd
  pushd src/mongo/db/modules/eloq/log_service
  git checkout ${LOG_SERVICE_HASH}
  git submodule update --recursive
  popd
else
  pushd src/mongo/db/modules/eloq/tx_service/raft_host_manager
  git checkout main
  git pull origin main
  popd
  pushd src/mongo/db/modules/eloq/log_service
  git checkout main
  git pull origin main
  git submodule update --recursive
  popd
fi
