DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
PATH="$PATH:/data/multiversion"

if [[ "${require_multiversion_setup}" = "true" && -n "${multiversion_exclude_tags_version}" ]]; then
  exclude_tags_file_path_arg=""
  if [ ! -d generated_resmoke_config ]; then
    exclude_tags_file_path_arg="--excludeTagsFilePath=multiversion_exclude_tags.yml"
  fi

  eval $python buildscripts/resmoke.py generate-multiversion-exclude-tags --oldBinVersion="${multiversion_exclude_tags_version}" "$exclude_tags_file_path_arg"
fi
