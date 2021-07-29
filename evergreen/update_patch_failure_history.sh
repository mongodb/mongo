DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit

activate_venv
git clone git@github.com:10gen/coredb-patchbuild-optimizer.git

pushd coredb-patchbuild-optimizer
# Copy the Evergreen config file into the working directory.
cp ../.evergreen.yml .

$python -m pip install tornado==6.1 motor==2.4
# Reusing bfsuggestion's password here to avoid having to
# go through the process of adding a new Evergreen project expansion.
$python -m patchbuild_optimizer "${bfsuggestion_password}"
popd
