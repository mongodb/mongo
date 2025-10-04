set -o errexit

BASEDIR=$(dirname "$0")
cd "$BASEDIR/../"

yamllint -c etc/yamllint_config.yml buildscripts etc jstests

PATH="$PATH:$HOME" evergreen evaluate etc/evergreen.yml >etc/evaluated_evergreen.yml
PATH="$PATH:$HOME" evergreen evaluate etc/evergreen_nightly.yml >etc/evaluated_evergreen_nightly.yml

# Remove references to the DSI repo before evergreen evaluate.
# The DSI module references break 'evaluate', the system_perf config should
# parse without them, and we don't want changes to the DSI repository to
# break checking that the rest of the imports etc. work.
awk '/lint_yaml trim start/{drop=1} /lint_yaml trim end/{drop=0} !drop' etc/system_perf.yml >etc/trimmed_system_perf.yml
PATH="$PATH:$HOME" evergreen evaluate etc/trimmed_system_perf.yml >etc/evaluated_system_perf.yml

python -m evergreen_lint -c ./etc/evergreen_lint.yml lint
