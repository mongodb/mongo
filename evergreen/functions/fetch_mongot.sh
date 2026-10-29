DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
set -o verbose

# Fetch the mongot binary for the tasks that need it, instead of packing it into every dist-test
# archive via the build_mongot expansion. Only a handful of tasks per variant run against a real
# mongot, while every task on the variant pays for a larger mongo-binaries.tgz, so the download
# belongs here.
#
# On variants that set build_mongot, mongot is already in the dist-test archive (the AL2023 mongot
# integration variants do this because their other mongot_e2e tasks need it), so there is nothing
# to fetch.
if [[ -x "${install_dir}/mongot-localdev/mongot" ]]; then
    echo "mongot already present at ${install_dir}/mongot-localdev/mongot, skipping download"
    exit 0
fi

# setup_db_contrib_tool runs evergreen/download_db_contrib_tool.py, which needs the venv's $python
# and a cwd of src. It installs db-contrib-tool into ${workdir}/bin, which prelude.sh puts on PATH.
activate_venv
setup_db_contrib_tool

# use_db_contrib_tool_mongot creates ./mongot-localdev relative to the current directory. Resmoke
# resolves mongot as ${install_dir}/mongot-localdev/mongot (see buildscripts/resmokelib/
# configure_resmoke.py), and get_mongot_version.sh reads the same path, so run it from install_dir
# rather than the repo root.
cd "${install_dir}"
use_db_contrib_tool_mongot
