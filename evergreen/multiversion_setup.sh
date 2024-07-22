DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
setup_db_contrib_tool

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

base_command="db-contrib-tool setup-repro-env"
evergreen_args="--installDir /data/install \
  --linkDir /data/multiversion \
  --platform $platform \
  --architecture $architecture \
  --evgVersionsFile multiversion-downloads.json"
local_args="--edition $edition \
  --fallbackToMaster \
  --resmokeCmd \"python buildscripts/resmoke.py\" \
  --debug \
  ${last_continuous_arg} \
  4.4 5.0.28"

remote_invocation="${base_command} ${evergreen_args} ${local_args}"
eval "${remote_invocation}"
echo "Verbatim db-contrib-tool invocation: ${remote_invocation}"

local_invocation="${base_command} ${local_args}"

if [ ! -z "${multiversion_platform_50_or_later}" ]; then
  platform="${multiversion_platform_50_or_later}"
fi

evergreen_args="--installDir /data/install \
  --linkDir /data/multiversion \
  --platform $platform \
  --architecture $architecture \
  --evgVersionsFile multiversion-downloads.json"
local_args="--edition $edition \
  --fallbackToMaster \
  --resmokeCmd \"python buildscripts/resmoke.py\" \
  --debug \
  ${last_lts_arg} 5.0 6.0"

remote_invocation="${base_command} ${evergreen_args} ${local_args}"
eval "${remote_invocation}"
echo "Verbatim db-contrib-tool invocation: ${remote_invocation}"

local_invocation="${local_invocation} && ${base_command} ${local_args}"
echo "Local db-contrib-tool invocation: ${local_invocation}"

echo "${local_invocation}" > local-db-contrib-tool-invocation.txt
