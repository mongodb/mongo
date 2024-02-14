set -o errexit

BASEDIR=$(dirname "$0")
cd "$BASEDIR/../"

yamllint -c etc/yamllint_config.yml buildscripts etc jstests

PATH="$PATH:$HOME" evergreen evaluate $EVERGREEN_CONFIG_FILE_PATH > etc/evaluated_evergreen.yml
python -m evergreen_lint -c ./etc/evergreen_lint.yml lint
