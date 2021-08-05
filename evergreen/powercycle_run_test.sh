DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

if [ "Windows_NT" = "$OS" ]; then
  user=Administrator
else
  user=$USER
fi

activate_venv
# Set an exit trap so we can save the real exit status (see SERVER-34033).
trap 'echo $? > error_exit.txt; exit 0' EXIT
set +o errexit
eval $python -u buildscripts/resmoke.py powercycle run \
  "--sshUserHost=$(printf "%s@%s" "$user" "${private_ip_address}") \
  --sshConnection=\"-i powercycle.pem\" \
  --taskName=${task_name} \
  ${run_powercycle_args}"
