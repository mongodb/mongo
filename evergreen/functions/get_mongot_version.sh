DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src
echo "Currently running mongot version: $(./${install_dir}/mongot-localdev/mongot --version)"
