#
# Download the MongoDB Database tools so that they are available for use in jstests
#

set -ex

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
source "$DIR/functions/get_mongodb_tools_url.sh"

mkdir -p mongodb_database_tools
pushd mongodb_database_tools

database_tools_url="$(get_mongodb_tools_url 100.14.1)" || exit 1
# Place the tools under mongodb_database_tools/bin in the root evergreen directory
curl ${database_tools_url} | tar xvz --strip-components=1

popd
