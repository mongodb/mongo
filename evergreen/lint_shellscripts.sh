DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o pipefail

cd src

PATH="/opt/shfmt/v3.2.4/bin:$PATH"
./buildscripts/shellscripts-linters.sh | tee shellscripts.log
exit_code=$?

activate_venv
$python ./buildscripts/simple_report.py --test-name shfmt --log-file shellscripts.log --exit-code $exit_code
exit $exit_code
