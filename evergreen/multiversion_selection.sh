DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
setup_db_contrib_tool

export PIPX_HOME="${workdir}/pipx"
export PIPX_BIN_DIR="${workdir}/pipx/bin"
export PATH="$PATH:$PIPX_BIN_DIR"

edition="${multiversion_edition}"
platform="${multiversion_platform}"
architecture="${multiversion_architecture}"

last_lts_arg="last-lts"
last_continuous_arg="last-continuous"

if [[ -n "${last_lts_evg_version_id}" ]]; then
    last_lts_arg="${last_lts_evg_version_id}=last-lts"
fi

if [[ -n "${last_continuous_evg_version_id}" ]]; then
    last_continuous_arg="${last_continuous_evg_version_id}=last-continuous"
fi

# Download last-continuous version separately on future-git-tag variant
if [[ -n "${multiversion_last_continuous_variant}" ]]; then
    last_continuous_arg=""
fi

base_command="db-contrib-tool setup-repro-env"
evergreen_args="-sb \
  --platform $platform \
  --architecture $architecture \
  --evgVersionsFile multiversion-downloads.json"
local_args="--edition $edition \
  --debug \
  --fallbackToMaster \
  ${last_lts_arg} \
  ${last_continuous_arg} 6.0 7.0"

remote_invocation="${base_command} ${evergreen_args} ${local_args}"
eval "${remote_invocation}"
echo "Verbatim db-contrib-tool invocation: ${remote_invocation}"

local_invocation="${base_command} ${local_args}"
echo "Local db-contrib-tool invocation: ${local_invocation}"

echo "${local_invocation}" >local-db-contrib-tool-invocation.txt

# Download last-continuous version from a dedicated variant on future-git-tag variant
if [[ -n "${multiversion_last_continuous_variant}" ]]; then
    last_continuous_arg="${version_id}=last-continuous"

    if [[ -n "${last_continuous_evg_version_id}" ]]; then
        last_continuous_arg="${last_continuous_evg_version_id}=last-continuous"
    fi

    future_git_tag_args="-sb \
    --variant ${multiversion_last_continuous_variant} \
    --evgVersionsFile multiversion-downloads.json \
    --debug \
    ${last_continuous_arg}"

    remote_invocation="${base_command} ${future_git_tag_args}"
    eval "${remote_invocation}"
    echo "Verbatim db-contrib-tool invocation: ${remote_invocation}"
fi
