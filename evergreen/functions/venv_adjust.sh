DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude_python.sh"

set -o errexit
set -o verbose

python_loc=$(which ${python})
pushd venv
venv_dir=$(pwd)
popd

# Update virtual env directory in activate script
if [ "Windows_NT" = "$OS" ]; then
  sed -i -e "s:VIRTUAL_ENV=\".*\":VIRTUAL_ENV=\"$venv_dir\":" "$venv_dir/Scripts/activate"
else
  sed -i -e "s:VIRTUAL_ENV=\".*\":VIRTUAL_ENV=\"$venv_dir\":" "$venv_dir/bin/activate"
fi

# Add back python symlinks on linux platforms
if [ "Windows_NT" = "$OS" ]; then
  exit 0
fi

cd "$venv_dir/bin"

rm python python3
ln -s "$python_loc" python3
ln -s python3 python
