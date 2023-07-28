DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

cd src/antithesis/antithesis_config/${suite}
sudo docker-compose up -d
sudo docker exec workload /bin/bash -c "cd resmoke && . python3-venv/bin/activate && python3 buildscripts/resmoke.py run --suite antithesis_${suite}"
