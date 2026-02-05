# Conditionally runs buildscripts/resmokelib/hang_analyzer/gen_hang_analyzer_tasks.py, if core dumps are present.
set -o errexit
set -o verbose

# Check if test failures exist before proceeding. This expansion is created in fetch_remote_test_results.sh.
# We should only trigger core analysis if there are test failures.
if [ "${test_failures_exist}" != "true" ]; then
    echo "No test failures detected (test_failures_exist: ${test_failures_exist}). Skipping core analysis task generation."
    exit 0
fi

# Check if there are any core dumps present before proceeding. Presence of a core dump here
# does not necessarily mean a core analysis task will be generated, just that the python
# script will run. It has more conditional logic within it. This check is implemented here
# to avoid needing to setup the Python virtual environment for every results tasks.

# Search for core files in ${workdir}/results/**/test.outputs/ directories
results_dir="${workdir}/results"
if [ ! -d "$results_dir" ]; then
    echo "No results directory found at $results_dir. Skipping core analysis task generation."
    exit 0
fi

# Look for *.core or *.mdmp files in results/**/test.outputs/ directories
core_dumps_found=false
while IFS= read -r -d '' test_outputs_dir; do
    if compgen -G "${test_outputs_dir}/*.core" >/dev/null || compgen -G "${test_outputs_dir}/*.mdmp" >/dev/null; then
        core_dumps_found=true
        break
    fi
done < <(find "$results_dir" -type d -name "test.outputs" -print0)

if [ "$core_dumps_found" = false ]; then
    echo "No core dumps found in $results_dir. Skipping core analysis task generation."
    exit 0
fi

echo "Core dumps found. Proceeding with core analysis task generation."

# Virtual environment setup is performed here, so that results tasks remain fast in the
# common case where there are no core dumps.
bash "${workdir}/src/evergreen/functions/venv_setup.sh"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

bash "${workdir}/src/evergreen/functions/evergreen_api_credentials_configure.sh"

cd src

activate_venv
echo $python
$python buildscripts/resmokelib/hang_analyzer/gen_hang_analyzer_tasks.py "$@"
