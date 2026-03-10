DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"
source "$DIR/functions/get_mongodb_tools_url.sh"

cd src

set -o errexit
set -o verbose
activate_venv

if [ -z "${build_patch_id}" ] || [ -z "${reuse_compile_from}" ] || [ "${is_patch:-false}" = "false" ]; then
    # Create target folder
    mkdir -p mongodb/

    # Generate feature flag list
    $python buildscripts/idl/gen_all_feature_flag_list.py turned-off-by-default
    mkdir -p mongodb/feature_flags
    cp ./all_feature_flags.txt mongodb/feature_flags

    # Generate server params list
    $python buildscripts/idl/gen_all_server_params_list.py
    mkdir -p mongodb/server_params
    cp ./all_server_params.txt mongodb/server_params

    binary_url="$(get_mongodb_tools_url 100.9.4)" || exit 1
    wget "$binary_url" -O mongo-tools.tar.gz
    tar -xzvf mongo-tools.tar.gz -C mongodb/ --strip-components=1 "mong*/bin"

    # generate atlas info
    uarch=$(uname -p)
    os=$(uname -r)
    json="{ \"version\": \"${version}\", \"gitVersion\": \"${revision}\", \"uarch\": \"$uarch\", \"os\": \"$os\" }"
    echo $json | jq '.' >mongodb/atlas_info.json

    # Add custom run_validate_collections.js wrapper
    mv jstests/hooks/run_validate_collections.js jstests/hooks/run_validate_collections.actual.js
    cat <<EOF >jstests/hooks/run_validate_collections.js
    print("NOTE: run_validate_collections.js will skip the oplog!");
    TestData = { skipValidationNamespaces: ['local.oplog.rs'] };
    await import("jstests/hooks/run_validate_collections.actual.js");
EOF

    # Copy the js tests
    mkdir -p mongodb/jstests/hooks
    cp -a jstests/* mongodb/jstests

    # Copy the build scripts
    mkdir -p mongodb/buildscripts
    cp -a buildscripts/* mongodb/buildscripts

    # Create the final archive
    tar czf supplementary-data.tgz mongodb
else
    # Evergreen does not handle nested escaped expansions well
    version_to_reuse_from=$(if [ -n "${build_patch_id}" ]; then echo "${build_patch_id}"; else echo "${reuse_compile_from}"; fi)
    curl -o supplementary-data.tgz https://s3.amazonaws.com/mciuploads/"${project}"/"${compile_variant}"/"${version_to_reuse_from}"/dsi/supplementary-data.tgz
fi
