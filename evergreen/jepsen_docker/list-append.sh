DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

# this file does not use set -euo pipefail because we determine test success or
# failure parsing the log file, instead of the return value.
# This whole script must run to ensure the report is generated and test
# artifacts are placed in the right location.

cd jepsen/docker

# actually run the tests
start_time=$(date +%s)
sudo docker exec jepsen-control bash --login -c "cd /jepsen/mongodb && lein run test-all -w list-append -n n1 -n n2 -n n3 -n n4 -n n5 -n n6 -n n7 -n n8 -n n9 -r 1000 --concurrency 3n --time-limit 240 --max-writes-per-key 128 --read-concern majority --write-concern majority --txn-read-concern snapshot --txn-write-concern majority --nemesis-interval 1 --nemesis partition --test-count 30" | tee jepsen_${task_name}_${execution}.log
end_time=$(date +%s)
elapsed_secs=$((end_time - start_time))

# copy files to expected locations for archiving
cd ../../
mkdir -p src/jepsen-mongodb
sudo docker cp jepsen-control:/jepsen/mongodb/store src/jepsen-mongodb/store
cp jepsen/docker/jepsen_${task_name}_${execution}.log src/jepsen-mongodb/
sudo docker cp jepsen-control:/jepsen/mongodb src/jepsen-workdir

cd src
activate_venv
$python buildscripts/jepsen_report.py --start_time=$start_time --end_time=$end_time --elapsed=$elapsed_secs --emit_status_files --store ./jepsen-mongodb jepsen-mongodb/jepsen_${task_name}_${execution}.log
exit_code=$?
cat report.json

if [ -f "jepsen_system_fail.txt" ]; then
  mv jepsen_system_fail.txt jepsen-mongodb/jepsen_system_failure_${task_name}_${execution}
  exit 0
fi

exit $exit_code
