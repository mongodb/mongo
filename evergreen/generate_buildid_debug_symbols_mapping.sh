DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

$python buildscripts/debugsymb_mapper.py \
  --version "${version_id}" \
  --client-id "${symbolizer_client_id}" \
  --client-secret "${symbolizer_client_secret}" \
  --variant "${build_variant}"
