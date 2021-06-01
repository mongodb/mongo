set -o errexit

BASEDIR=$(dirname "$0")
cd "$BASEDIR/../"

find buildscripts etc jstests -name '*.y*ml' -exec yamllint -c etc/yamllint_config.yml {} +
python -m evergreen_lint -c ./etc/evergreen_lint.yml lint
