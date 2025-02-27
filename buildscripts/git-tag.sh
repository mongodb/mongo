#!/usr/bin/bash
set -exo
TAG=$1

pushd src/mongo/db/modules/eloq/tx_service/raft_host_manager
RAFT_HOST_MGR_HASH=$(git rev-parse HEAD)
popd
pushd src/mongo/db/modules/eloq/log_service
LOG_SERVICE_HASH=$(git rev-parse HEAD)
popd
echo "raft_host_manager=${RAFT_HOST_MGR_HASH}" >.private_modules
echo "log_service=${LOG_SERVICE_HASH}" >>.private_modules
if [ -n "$(git diff --name-only .private_modules)" ]; then
    git add .private_modules
    git commit -m "New tag ${TAG}"
fi
git tag "${TAG}"
git push
git push origin "${TAG}"
