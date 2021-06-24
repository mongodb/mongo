DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

# Trigger a system failure if powercycle failed due to ssh access.
if [ -n "${ec2_ssh_failure}" ]; then
  echo "ec2_ssh_failure detected - $(cat powercycle_exit.yml)"
  exit ${exit_code}
fi
