set -o errexit

BASEDIR=$(dirname "$0")
cd "$BASEDIR/../"

find buildscripts etc jstests -name '*.y*ml' -exec yamllint -c etc/yamllint_config.yml {} +

# TODO: SERVER-64923 re-enable YAML linters.
#evergreen evaluate ${evergreen_config_file_path} > etc/evaluated_evergreen.yml
#python -m evergreen_lint -c ./etc/evergreen_lint.yml lint
