DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

setup_db_contrib_tool_venv

export PIPX_HOME="${workdir}/pipx"
export PIPX_BIN_DIR="${workdir}/pipx/bin"
export PATH="$PATH:$PIPX_BIN_DIR"

rm -rf /data/install /data/multiversion

edition="${multiversion_edition}"
platform="${multiversion_platform}"
architecture="${multiversion_architecture}"

last_lts_arg="--installLastLTS"
last_continuous_arg="--installLastContinuous"

if [[ -n "${last_lts_evg_version_id}" ]]; then
  last_lts_arg="${last_lts_evg_version_id}"
fi

if [[ -n "${last_continuous_evg_version_id}" ]]; then
  last_continuous_arg="${last_continuous_evg_version_id}"
fi

db-contrib-tool setup-repro-env \
  --installDir /data/install \
  --linkDir /data/multiversion \
  --edition $edition \
  --platform $platform \
  --architecture $architecture \
  --fallbackToMaster \
  --resmokeCmd "python buildscripts/resmoke.py" \
  --debug \
  $last_lts_arg \
  $last_continuous_arg 4.4 5.0
