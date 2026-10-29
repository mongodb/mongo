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

# Skip the mainline last-lts download. On a branch where last_lts_fcv == last_continuous_fcv (both
# 9.0 on master today), the bare `last-lts` request resolves to the mainline v9.0 build and gets
# the SAME `mongod-9.0` bin suffix as the last-continuous release. Both are symlinked
# into the link dir under that one name and the last writer wins -- nondeterministically, since
# db-contrib-tool iterates its download requests out of a set. The mainline build has no atlas
# module, so when it wins the old node dies with "Unknown --setParameter
# 'disaggregatedStorageConfig'". Variants that set `last_versions` without `last_lts` (e.g. the
# disagg variants' `last_patch`) need no last-lts binary at all, so drop the request.
if [[ -n "${last_versions}" && "${last_versions}" != *"last_lts"* ]]; then
    last_lts_arg=""
fi

# Optionally include the last-patch version (e.g. on the disagg variant). The release feed source
# used to resolve it is controlled independently via the DB_CONTRIB_TOOL_RELEASE_FEED_SOURCE env
# variable (db-contrib-tool's --releaseFeedSource option), injected by the evergreen function.
last_patch_arg=""
if [[ -n "${multiversion_include_last_patch}" ]]; then
    last_patch_arg="last-patch"
fi

# For the disagg variants (atlas feed source), resolve the last DSC release
# dynamically. The resolver picks the newest release whose DSC binary is published in the atlas
# feed for this platform and that is older than the version under test; its stdout is exactly the
# version string, which db-contrib-tool matches against PATCH_VERSION_RE and looks up by exact
# version key in the release feed selected by DB_CONTRIB_TOOL_RELEASE_FEED_SOURCE.
#
# The release is injected through the last-patch slot: the suite's old_bin_version: last_patch
# resolves the binary name from the git-tag machinery (the same series as the DSC release), and the
# =last-patch suffix symlinks it accordingly. The variant generates no last-continuous sub-tasks,
# so the last-continuous request is dropped.
#
# An explicit multiversion_dsc_release expansion overrides discovery (emergency escape hatch, e.g.
# to pin an older DSC release candidate); the resolver echoes it verbatim.
resolver_args=()
if [[ -n "${multiversion_dsc_release}" ]]; then
    resolver_args+=(--override "${multiversion_dsc_release}")
fi
if [[ "${multiversion_release_feed_source}" == "atlas" ]]; then
    resolver_output="$(${python} evergreen/resolve_dsc_release.py --debug ${resolver_args[@]+"${resolver_args[@]}"})" &&
        resolver_exit=0 || resolver_exit=$?
    if [[ ${resolver_exit} -ne 0 || -z "${resolver_output}" ]]; then
        echo "ERROR: could not resolve a DSC release for last-patch (resolver exit ${resolver_exit});"
        echo "the resolver's stderr above has the reason. To pin a version manually, set the"
        echo "multiversion_dsc_release expansion."
        exit 1
    fi
    last_patch_arg="${resolver_output}=last-patch"
    last_continuous_arg=""
    echo "resolved last-patch DSC release: ${resolver_output}"
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
  7.0 8.0 8.0.16"

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
