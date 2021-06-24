DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/build

set -o verbose
set -o errexit

# If this is a patch build, blow away the file so our subsequent and optional s3.put
# doesn't run. That way, we won't overwrite the latest part in our patches.
if [ "${is_patch}" = "true" ]; then
  rm -f src/build/embedded-sdk.tgz
fi
