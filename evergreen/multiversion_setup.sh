DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
setup_db_contrib_tool

export PIPX_HOME="${workdir}/pipx"
export PIPX_BIN_DIR="${workdir}/pipx/bin"
export PATH="$PATH:$PIPX_BIN_DIR"

rm -rf /data/install /data/multiversion

command="db-contrib-tool setup-repro-env multiversion-downloads.json --installDir /data/install --linkDir /data/multiversion --debug"
echo "Verbatim db-contrib-tool invocation: ${command}"

eval "${command}"
