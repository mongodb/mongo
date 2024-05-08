DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

if [ "${task_name}" != "sasl_windows_cyrussasl" ]; then
  exit 0
fi

echo "Cleaning up Windows CyrusSASL Test Artifacts"

readonly k_cyrussasl_default_dir_root="/cygdrive/c/CMU"

if [[ ! -d "$k_cyrussasl_default_dir_root" ]]; then
  echo "Could not find $k_cyrussasl_default_dir_root to cleanup..."
  exit 0
fi

rm -rf "$k_cyrussasl_default_dir_root"
echo "Deleted $k_cyrussasl_default_dir_root from host"
