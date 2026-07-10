set -euo pipefail

cd jepsen/docker
./bin/up -n 6 --storage-engine sls --sls-node-count 3 -d >docker.log 2>&1
