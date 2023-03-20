set -o errexit

BASEDIR=$(dirname "$0")
cd "$BASEDIR/../"

yamllint -c etc/yamllint_config.yml buildscripts etc jstests
