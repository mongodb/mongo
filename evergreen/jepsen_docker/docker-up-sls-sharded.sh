set -euo pipefail

# Bring up a 12-node SLS cluster: 9 mongod nodes (n1-n9) plus 3 SLS service
# nodes (n10-n12). The sharded topology is selected at test time (see
# list-append-sls-sharded.sh).
cd jepsen/docker
./bin/up -n 12 --storage-engine sls --sls-node-count 3 -d >docker.log 2>&1
