DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o verbose
set -o errexit

activate_venv
$python buildscripts/evergreen_resmoke_job_count.py \
  --taskName ${task_name} \
  --buildVariant ${build_variant} \
  --distro ${distro_id} \
  --jobFactor ${resmoke_jobs_factor} \
  --jobsMax ${resmoke_jobs_max} \
  --outFile resmoke_jobs_expansion.yml
