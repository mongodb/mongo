DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/jepsen-mongodb

set -o verbose
# Jepsen system failure if file exists.
if [ -f jepsen_system_failure_${task_name}_${execution} ]; then
  exit $(cat jepsen_system_failure_${task_name}_${execution})
fi
