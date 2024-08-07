set -o errexit

BASEDIR=$(dirname "$0")
cd "$BASEDIR/../"

yamllint -c etc/yamllint_config.yml buildscripts etc jstests

PATH="$PATH:$HOME" evergreen evaluate etc/evergreen.yml > etc/evaluated_evergreen.yml
PATH="$PATH:$HOME" evergreen evaluate etc/evergreen_nightly.yml > etc/evaluated_evergreen_nightly.yml
PATH="$PATH:$HOME" evergreen evaluate etc/system_perf.yml > etc/evaluated_system_perf.yml

python -m evergreen_lint -c ./etc/evergreen_lint.yml lint
