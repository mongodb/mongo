DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
if [ "Windows_NT" = "$OS" ]; then
  vcvars="$(vswhere -latest -property installationPath | tr '\\' '/' | dos2unix.exe)/VC/Auxiliary/Build/"
  echo "call \"$vcvars/vcvarsall.bat\" amd64" > msvc.bat
  for i in "${compile_env[@]}"; do
    echo "set $i" >> msvc.bat
  done
  echo "ninja -f ${ninja_file} install-core" >> msvc.bat
  cmd /C msvc.bat
else
  eval ${compile_env} ninja -f ${ninja_file} install-core compiledb
fi
