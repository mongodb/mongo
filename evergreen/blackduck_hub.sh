DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

# activate the virtualenv if it has been set up
activate_venv

additional_args=
if [ "$branch_name" != "master" ]; then
  additional_args="--vulnerabilities_only"
fi

python buildscripts/blackduck_hub.py -v scan_and_report --build_logger=mci.buildlogger --build_logger_task_id=${task_id} --report_file=report.json $additional_args
