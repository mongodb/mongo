DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

# Retry the build since it's deterministic and may fail due to transient issues
# (e.g. flaky network to the build cache, or transient toolchain/OOM errors). A
# genuine build error is deterministic and will still fail the task once the
# retries are exhausted. Relax errexit so a failed attempt doesn't abort the
# script before we can retry; capture the status and restore errexit afterward.
set +o errexit
exit_status=1
for i in {1..3}; do
  if [ "Windows_NT" = "$OS" ]; then
    vcvars="$(vswhere -latest -property installationPath | tr '\\' '/' | dos2unix.exe)/VC/Auxiliary/Build/"
    echo "call \"$vcvars/vcvarsall.bat\" amd64" > msvc.bat
    for env_var in "${compile_env[@]}"; do
      echo "set $env_var" >> msvc.bat
    done
    echo "ninja -f ${ninja_file} ${targets}" >> msvc.bat
    cmd /C msvc.bat
  else
    eval ${compile_env} ninja -f ${ninja_file} ${targets}
  fi
  exit_status=$?
  if [[ $exit_status -eq 0 ]]; then
    break
  fi
  if [[ $i -lt 3 ]]; then
    echo "Ninja build failed (attempt ${i}/3, exit ${exit_status}), retrying..."
    sleep 5
  else
    echo "Ninja build failed (attempt ${i}/3, exit ${exit_status})."
  fi
done
set -o errexit

exit $exit_status
