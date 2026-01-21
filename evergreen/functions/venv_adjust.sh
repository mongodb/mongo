DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude_python.sh"

set -o errexit
set -o verbose

python_loc=$(which ${python})
pushd venv
venv_dir=$(pwd)
popd

ARCH=$(uname -m)
if [[ "$ARCH" == "arm64" || "$ARCH" == "aarch64" ]]; then
    ARCH="arm64"
elif [[ "$ARCH" == "ppc64le" || "$ARCH" == "ppc64" || "$ARCH" == "ppc" || "$ARCH" == "ppcle" ]]; then
    ARCH="ppc64le"
elif [[ "$ARCH" == "s390x" || "$ARCH" == "s390" ]]; then
    ARCH="s390x"
else
    ARCH="x86_64"
fi

# TODO SERVER-105520
# try using downloaded venv once more reliability has been built into venv upload/download
if [[ "$ARCH" == "ppc64le" ]]; then
    rm -rf $venv_dir
    source "$DIR/venv_setup.sh"
else
    # Update virtual env directory in activate script
    if [ "Windows_NT" = "$OS" ]; then
        # Update VIRTUAL_ENV paths in activate script
        # Python 3.10 format: VIRTUAL_ENV="C:\path\to\venv"
        # Python 3.13 format: VIRTUAL_ENV=$(cygpath 'C:\path\to\venv') and export VIRTUAL_ENV='C:\path\to\venv'
        # Use multiple sed patterns to handle both formats
        sed -i -e "s:VIRTUAL_ENV=\".*\":VIRTUAL_ENV=\"$venv_dir\":" \
            -e "s:VIRTUAL_ENV='.*':VIRTUAL_ENV='$venv_dir':" \
            -e "s:VIRTUAL_ENV=\$(cygpath ['\"].*['\"])$:VIRTUAL_ENV=\$(cygpath '$venv_dir'):" \
            "$venv_dir/Scripts/activate"
    else
        # Update VIRTUAL_ENV paths in activate script
        # Python 3.10 format: VIRTUAL_ENV="/path" (double quoted)
        # Python 3.13 format: export VIRTUAL_ENV=/path (unquoted with export)
        sed -e "s:VIRTUAL_ENV=\".*\":VIRTUAL_ENV=\"$venv_dir\":" \
            -e "s:export VIRTUAL_ENV=/.*:export VIRTUAL_ENV=$venv_dir:" \
            "$venv_dir/bin/activate" >"$venv_dir/bin/activate.tmp"
        mv "$venv_dir/bin/activate.tmp" "$venv_dir/bin/activate"
    fi

    # Add back python symlinks on linux platforms
    if [ "Windows_NT" = "$OS" ]; then
        exit 0
    fi

    cd "$venv_dir/bin"

    rm python python3
    ln -s "$python_loc" python3
    ln -s python3 python
fi
