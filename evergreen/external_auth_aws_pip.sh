DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose
activate_venv

# Not all git get project calls clone into ${workdir}/src so we allow
# callers to tell us where the pip requirements files are.
pip_dir="${pip_dir}"
if [[ -z $pip_dir ]]; then
  # Default to most common location
  pip_dir="${workdir}/src/etc/pip"
fi

# Same as above we have to use quotes to preserve the
# Windows path separator
external_auth_txt="$pip_dir/components/aws.req"
python -m pip install -r "$external_auth_txt"
