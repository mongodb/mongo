#!/bin/bash
# Initial venv setup for Streams tasks that only need basic Python packages.
# Installs only click and pyyaml (required for prelude.sh expansions parsing).
# This is much faster than the full venv setup (~5s vs ~47s).

set -o errexit

evergreen_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)/.."
. "$evergreen_dir/prelude_workdir.sh"
. "$evergreen_dir/prelude_python.sh"

python_loc=$(which ${python})
echo "python_loc set to $python_loc"

venv_dir="${workdir}/venv"

if [ -d "$venv_dir" ]; then
    echo "Venv already exists, skipping streams initial setup"
    exit 0
fi

"$python_loc" -m venv "$venv_dir"
. "${venv_dir}/bin/activate"

echo "Installing initial packages (click, pyyaml)..."
python -m pip --disable-pip-version-check install --quiet --no-cache-dir click pyyaml

echo "Streams initial venv setup complete"
