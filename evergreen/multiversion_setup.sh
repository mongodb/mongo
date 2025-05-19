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

link_dir="${multiversion_link_dir}"
install_dir="${multiversion_install_dir}"
rm -rf $link_dir $install_dir

command="db-contrib-tool setup-repro-env multiversion-downloads.json --installDir $install_dir --linkDir $link_dir --debug"
echo "Verbatim db-contrib-tool invocation: ${command}"

eval "${command}"
