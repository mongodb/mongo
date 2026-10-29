DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
setup_db_contrib_tool

link_dir="${multiversion_link_dir}"
install_dir="${multiversion_install_dir}"
rm -rf $link_dir $install_dir

# multiversion-downloads.json is resolved once per build variant by select_multiversion_binaries
# and lists every version any suite on the variant might need, so handing it to db-contrib-tool
# downloads all of them. When the task generator told us which versions this task actually needs,
# trim the file to those. When it did not -- an older generator, or a task that tests against
# several versions -- the file is left alone and everything is downloaded as before.
#
# Set multiversion_filter_downloads to anything other than "true" to disable it.
if [[ "${multiversion_filter_downloads}" == "true" ]]; then
    $python buildscripts/multiversion_downloads_filter.py \
        --input multiversion-downloads.json \
        --versions "${multiversion_setup_versions}"
fi

command="db-contrib-tool setup-repro-env multiversion-downloads.json --installDir $install_dir --linkDir $link_dir --debug"
echo "Verbatim db-contrib-tool invocation: ${command}"

eval "${command}"
