DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o pipefail
set -o verbose

cd src
bash buildscripts/clang_tidy.sh ${clang_tidy_toolchain} | tee clang-tidy.log
exit_code=$?

activate_venv
$python ./buildscripts/simple_report.py --test-name clang_tidy --log-file clang-tidy.log --exit-code $exit_code --dedup-lines
echo $?
exit $exit_code
