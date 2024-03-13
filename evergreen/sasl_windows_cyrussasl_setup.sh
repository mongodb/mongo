DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

readonly k_cyrussasl_plugin_filename="cyrus_sasl_windows_test_plugin.dll"
readonly k_cyrussasl_plugin_dir="/cygdrive/c/CMU/bin/sasl2"

plugin_path="$(find . -name "*${k_cyrussasl_plugin_filename}")"

if [[ -z "$plugin_path" ]]; then
  echo >&2 "Could not find ${k_cyrussasl_plugin_filename} from path '$(pwd)' !"
  exit 1
fi

echo "Configuring CyrusSASL plugin .dll from '$plugin_path'"

mkdir -p "$k_cyrussasl_plugin_dir"

cp "$plugin_path" "$k_cyrussasl_plugin_dir"
