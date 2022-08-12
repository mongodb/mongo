DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose
activate_venv

$python buildscripts/idl/check_stable_api_commands_have_idl_definitions.py -v --install-dir dist-test/bin --include src --include src/mongo/db/modules/enterprise/src 1
$python buildscripts/idl/checkout_idl_files_from_past_releases.py -v idls

function run_idl_check_compatibility {
  dir=$1
  output=$(
    python buildscripts/idl/idl_check_compatibility.py -v \
      --old-include "$dir/src" \
      --old-include "$dir/src/mongo/db/modules/enterprise/src" \
      --new-include src \
      --new-include src/mongo/db/modules/enterprise/src \
      "$dir/src" src
  )
  exit_code=$?
  echo "Performing idl check compatibility with release: $dir:"
  echo "$output"
  if [ $exit_code -ne 0 ]; then
    exit 255
  fi
}
export -f run_idl_check_compatibility
find idls -maxdepth 1 -mindepth 1 -type d | xargs -n 1 -P 0 -I % bash -c 'run_idl_check_compatibility "$@"' _ %

# Run the idl compatibility checker script with the current src directory as both the "old" and
# "new" versions. This is so that we can check that commands that were newly added since the last
# release adhere to compatibility requirements.
echo "Performing idl check compatibility for newly added commands since the last release:"
$python buildscripts/idl/idl_check_compatibility.py -v --old-include src --old-include src/mongo/db/modules/enterprise/src --new-include src --new-include src/mongo/db/modules/enterprise/src src src
