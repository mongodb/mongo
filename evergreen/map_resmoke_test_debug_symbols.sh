# Posts 'buildId -> debug symbols URL' mappings for the server binaries that the
# resmoke_tests task built itself.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

if [ ! -d dist-tests/bin ] || [ -z "$(ls -A dist-tests/bin 2>/dev/null)" ]; then
    echo "No relinked test binaries in dist-tests/bin, skipping debug symbol mapping."
    exit 0
fi

activate_venv

set +o errexit
$python buildscripts/debugsymb_mapper.py \
    --version "${version_id}" \
    --variant "${build_variant}" \
    --client-id "${symbolizer_client_id}" \
    --client-secret "${symbolizer_client_secret}" \
    --binaries-dir dist-tests \
    --task-id "${task_id}" \
    --mongodb-version "${version}"
ret=$?
set -o errexit

if [ "$ret" -ne 0 ]; then
    echo "WARNING: failed to post build ID to debug symbol mappings (exit ${ret}). Stack traces"
    echo "from this task may not be symbolizable. This does not affect the test results above."
fi

exit 0
