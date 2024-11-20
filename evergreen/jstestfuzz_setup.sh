DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

for i in {1..5}; do
  git clone https://x-access-token:${github_token}@github.com/10gen/jstestfuzz.git && RET=0 && break || RET=$? && sleep 5
  echo "Failed to clone github.com:10gen/jstestfuzz.git, retrying..."
done

if [ $RET -ne 0 ]; then
  echo "Failed to clone git@github.com:10gen/jstestfuzz.git"
  exit $RET
fi
