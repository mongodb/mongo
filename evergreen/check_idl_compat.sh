DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose
activate_venv

$python buildscripts/idl/check_stable_api_commands_have_idl_definitions.py -v --install-dir dist-test/bin --include src --include src/mongo/db/modules/enterprise/src 1
$python buildscripts/idl/checkout_idl_files_from_past_releases.py -v idls

mkdir -p idl_reports

function run_idl_check_compatibility {
    dir=$1
    set +o errexit
    output=$(
        python buildscripts/idl/idl_check_compatibility.py -v \
            --report-file "idl_reports/$(basename "$dir").json" \
            --old-include "$dir/src" \
            --old-include "$dir/src/mongo/db/modules/enterprise/src" \
            --new-include src \
            --new-include src/mongo/db/modules/enterprise/src \
            "$dir/src" src
    )
    exit_code=$?
    echo "Performing idl check compatibility with release: $dir:"
    echo "$output"
    if [ $exit_code -ne 0 ]; then
        exit 1
    fi
}
export -f run_idl_check_compatibility
exit_code=0
find idls -maxdepth 1 -mindepth 1 -type d | xargs -n 1 -P 0 -I % bash -c 'run_idl_check_compatibility "$@"' _ % || exit_code=$?

# Run the idl compatibility checker script with the current src directory as both the "old" and
# "new" versions. This is so that we can check that commands that were newly added since the last
# release adhere to compatibility requirements.
echo "Performing idl check compatibility for newly added commands since the last release:"
$python buildscripts/idl/idl_check_compatibility.py -v --report-file idl_reports/newly_added.json --report-label newly_added --old-include src --old-include src/mongo/db/modules/enterprise/src --new-include src --new-include src/mongo/db/modules/enterprise/src src src || exit_code=$?

# Merge per-release reports into a single Evergreen attach.results report (one test per error).
# A release whose checker died without writing a usable report (e.g. killed or timed out) gets a
# synthetic failing test, so that it cannot silently disappear from the results. This only covers
# the checkers dying; if the whole task is killed this step does not run.
$python - <<'PY'
import glob, json, os, time
results = []
expected = [os.path.basename(d) for d in glob.glob("idls/*") if os.path.isdir(d)] + ["newly_added"]
for name in sorted(expected):
    path = f"idl_reports/{name}.json"
    try:
        with open(path) as f:
            results.extend(json.load(f)["results"])
        continue
    except FileNotFoundError:
        reason = f"The IDL compatibility checker for {name} did not write {path}."
    except (ValueError, KeyError, TypeError) as exc:
        reason = f"The IDL compatibility report {path} for {name} is unreadable: {exc!r}"
    now = time.time()
    results.append(
        {
            "test_file": f"{name}/missing_report",
            "status": "fail",
            "start": now,
            "end": now,
            "log_raw": reason,
        }
    )
with open("report.json", "w") as f:
    json.dump({"results": results}, f)
PY
# The task-level post steps attach report.json (one test per error) even when this script fails.
exit $exit_code
