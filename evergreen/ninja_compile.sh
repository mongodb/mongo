DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

if [[ -n "${ninja_jobs}" ]]; then
  JOBS_ARG="-j${ninja_jobs}"
fi

if [ "Windows_NT" = "$OS" ]; then
  vcvars="$(vswhere -latest -property installationPath | tr '\\' '/' | dos2unix.exe)/VC/Auxiliary/Build/"
  echo "call \"$vcvars/vcvarsall.bat\" amd64" > msvc.bat
  for i in "${compile_env[@]}"; do
    echo "set $i" >> msvc.bat
  done
  echo "ninja -f ${ninja_file} ${JOBS_ARG} ${targets}" >> msvc.bat
  for i in $(seq ${ninja_retries:=1}); do
    cmd /C msvc.bat && RET=0 && break || RET=$? && sleep 1
  done
else
  for i in $(seq ${ninja_retries:=1}); do
    eval ${compile_env} ninja -f ${ninja_file} ${JOBS_ARG} ${targets} && RET=0 && break || RET=$? && sleep 1
  done
fi

if [ $RET -ne 0 ]; then
  echo "Ninja command failed"
  exit $RET
fi
