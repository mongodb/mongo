set -euo pipefail

cd jepsen/docker
./bin/up -n 9 -d 2>&1 > docker.log
