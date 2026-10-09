DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

build_patch_id="${build_patch_id:-${reuse_compile_from}}"
if [ -n "${build_patch_id}" ]; then
    exit 0
fi

# An ARM64 cross builder cannot execute its s390x output. The native
# record_s390x_server_version task publishes this metadata instead.
if [[ -n "${mongo_s390x_cross_config:-}" && "$(uname -m)" == "aarch64" ]]; then
    exit 0
fi

"$1" --version >"$2"
