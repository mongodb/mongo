DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

build_patch_id="${build_patch_id:-${reuse_compile_from}}"
if [ -n "${build_patch_id}" ]; then
    exit 0
fi

is_san_variant_arg=""
if [[ -n "${san_options}" ]]; then
    is_san_variant_arg="--is-san-variant"
fi

activate_venv

$python buildscripts/debugsymb_mapper.py \
    --version "${version_id}" \
    --client-id "${symbolizer_client_id}" \
    --client-secret "${symbolizer_client_secret}" \
    --variant "${build_variant}" \
    $is_san_variant_arg
