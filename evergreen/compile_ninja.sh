DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

activate_venv
if [ "Windows_NT" = "$OS" ]; then
  vcvars="$(vswhere -latest -property installationPath | tr '\\' '/' | dos2unix.exe)/VC/Auxiliary/Build/"
  cd "$vcvars" && cmd /K "vcvarsall.bat amd64 && cd ${workdir}\src"
fi
python -m pip install ninja
ninja install-core
