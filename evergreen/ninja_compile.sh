DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose
activate_venv
python -m pip install ninja
if [ "Windows_NT" = "$OS" ]; then
  vcvars="$(vswhere -latest -property installationPath | tr '\\' '/' | dos2unix.exe)/VC/Auxiliary/Build/"
  echo "call \"$vcvars/vcvarsall.bat\" amd64" > msvc.bat
  echo "ninja install-core" >> msvc.bat
  cmd /C msvc.bat
else
  ninja install-core
fi
