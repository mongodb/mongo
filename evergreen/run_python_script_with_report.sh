unset workdir
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o verbose

cd src

# Get the name of the test to be included in the report
report_test_name="$1"
# Shift the args so "$@" will unwind only the args to be passed to the python process
shift

activate_venv
echo $python $@
echo $python $@ > python_report.log
$python "$@" &>> python_report.log
exit_code=$?
echo "Finished with exit code: $exit_code" >> python_report.log

$python ./buildscripts/simple_report.py --test-name "${report_test_name}" --log-file python_report.log --exit-code $exit_code
exit $exit_code
