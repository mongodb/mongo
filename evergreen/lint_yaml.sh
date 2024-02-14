DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o pipefail

cd src

activate_venv
EVERGREEN_CONFIG_FILE_PATH=${evergreen_config_file_path} ./buildscripts/yamllinters.sh | tee yamllinters.log
exit_code=$?

$python ./buildscripts/simple_report.py --test-name yamllinters --log-file yamllinters.log --exit-code $exit_code
exit $exit_code
