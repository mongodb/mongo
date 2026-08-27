DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
setup_db_contrib_tool

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

# Include the last-patch version when this branch has last-patch coverage. Two ways in:
#
#  - multiversion_include_last_patch, a per-variant opt-in used by the disagg suites, which
#    resolve last-patch from the private atlas feed rather than from published releases. Those
#    deliberately test against release candidates, so they must NOT set the final-only policy
#    below -- restricting them to GA tags would leave them with nothing to resolve.
#
#  - resmoke reporting last_patch as a default peer, which is true exactly when the branch's
#    series has already shipped a release.
#
# Both checks sit inside the `if` so that `set -o errexit` cannot abort the whole selection
# step: failing to answer the question means "no last-patch", never a lost
# last-lts/last-continuous download.
last_patch_arg=""
if [[ -n "${multiversion_include_last_patch}" ]]; then
    last_patch_arg="last-patch"
elif $python buildscripts/resmoke.py multiversion-config \
    --config-file-output multiversion-config.yml &&
    $python -c "
import sys, yaml

with open('multiversion-config.yml') as handle:
    config = yaml.safe_load(handle) or {}
sys.exit(0 if 'last_patch' in (config.get('last_versions') or []) else 1)
"; then
    export MULTIVERSION_LAST_PATCH_FINAL_ONLY=1
    last_patch_arg="last-patch"
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
  ${last_continuous_arg} \
  ${last_patch_arg} \
  7.0 8.0.16"

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
