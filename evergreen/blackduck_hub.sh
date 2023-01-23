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

# BlackDuck crashes on this gzip file because it is not well-formed
# invalid compressed data--format violated
rm ./src/third_party/zstandard/zstd/tests/gzip/hufts-segv.gz

# Remove package.json since it only exists for vscode
# MongoDB server does not use Node.JS code so we strip this file to not confuse BlackDuck Detect
# Otherwise we need to run npm install to install everything in package.json or disable the NPM
# scanner.
rm package.json

python buildscripts/blackduck_hub.py -v scan_and_report --build_logger=mci.buildlogger --build_logger_task_id=${task_id} --report_file=report.json $additional_args
