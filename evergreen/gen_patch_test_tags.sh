DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

# Only generate tags for query patch variants.
if [[ "${build_variant}" != *"query-patch-only"* ]]; then
  exit 0
fi

activate_venv
git clone git@github.com:10gen/coredb-patchbuild-optimizer.git

pushd coredb-patchbuild-optimizer
# Reusing bfsuggestion's password here to avoid having to
# go through the process of adding a new Evergreen project expansion.
$python tagfilegenerator.py "${coredb_patchbuild_optimizer_password}"
mv failedtesttags ..
popd

tar -cvzf patch_test_tags.tgz failedtesttags
