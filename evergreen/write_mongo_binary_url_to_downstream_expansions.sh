DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

touch downstream_expansions.yaml
echo "${build_variant}-mongo-binaries: https://mciuploads.s3.amazonaws.com/${mongo_binaries}" 2>&1 | tee downstream_expansions.yaml
