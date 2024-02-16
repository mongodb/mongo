DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

# Compile without extra args from evergreen to better simulate local dev compilation.

cd src

set -o errexit
set -o verbose

if [ "Windows_NT" = "$OS" ]; then
  vcvars="$(vswhere -latest -property installationPath | tr '\\' '/' | dos2unix.exe)/VC/Auxiliary/Build/"
  export PATH="$(echo "$(cd "$vcvars" && cmd /C "vcvarsall.bat amd64 && C:/cygwin/bin/bash -c 'echo \$PATH'")" | tail -n +6)":$PATH
fi
activate_venv

set -o pipefail

echo "Setting evergreen tmp dir to $TMPDIR"
compile_flags="$compile_flags --evergreen-tmp-dir='${TMPDIR}'"

eval ${compile_env} $python ./buildscripts/scons.py \
  ${compile_flags} ${task_compile_flags} \
  ${targets} | tee scons_stdout.log

exit $?
