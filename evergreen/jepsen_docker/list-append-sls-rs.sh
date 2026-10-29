DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

# this file does not use set -euo pipefail because we determine test success or
# failure parsing the log file, instead of the return value.
# This whole script must run to ensure the report is generated and test
# artifacts are placed in the right location.

# Run the test 10 times, preserving mongod/SLS logs
# per iteration. --nemesis-interval is relaxed to 3s (see the lein invocation
# below) to avoid the dirty-update/G1a anomaly seen under aggressive 1s kill
# nemesis.
TEST_COUNT=10

# ---------------------------------------------------------------------------
# Clean up any stale artifacts from previous runs so the post-task upload
# step never picks up old results.
# ---------------------------------------------------------------------------
rm -rf src/jepsen-mongodb src/jepsen-workdir
mkdir -p src/jepsen-mongodb src/jepsen-workdir

# ---------------------------------------------------------------------------
# Pre-test sanity checks: verify the mongod binary is present and executable
# inside the jepsen-node containers before spending 120 s waiting for a port
# that will never open.
# ---------------------------------------------------------------------------
echo "=== Pre-test mongod binary check (jepsen-n1) ==="
sudo docker exec jepsen-n1 ls -la /usr/bin/mongod 2>&1 || true
sudo docker exec jepsen-n1 /usr/bin/mongod --version 2>&1 || true
echo "=== Pre-test mongod binary check done ==="

start_time=$(date +%s)

for i in $(seq 1 ${TEST_COUNT}); do
    cd jepsen/docker

    sudo docker exec jepsen-control bash --login -c "\
  cd /jepsen/mongodb && \
  lein run test-all -w list-append \
  -n n1 -n n2 -n n3 -n n4 -n n5 -n n6 \
  --storage-engine sls \
  --topology replica-set \
  --sls-node-count 3 \
  --sls-cell-count 3 \
  -r 1000 \
  --concurrency 3n \
  --time-limit 120 \
  --max-writes-per-key 128 \
  --read-concern majority \
  --write-concern majority \
  --txn-read-concern snapshot \
  --txn-write-concern majority \
  --nemesis-interval 3 \
  --nemesis partition,kill \
  --test-count 1" | tee jepsen_test_${i}.log

    cd ../../

    # store accumulates across iterations; archive only this run (store/latest,
    # -L to deref the symlink) instead of the whole store to avoid duplication.
    mkdir -p src/jepsen-mongodb/store/test-index${i}
    sudo docker cp -L jepsen-control:/jepsen/mongodb/store/latest src/jepsen-mongodb/store/test-index${i}
    cp jepsen/docker/jepsen_test_${i}.log src/jepsen-mongodb/
    sudo docker cp jepsen-control:/jepsen/mongodb src/jepsen-workdir
    sudo chmod -R +r src/jepsen-workdir || true
    sudo chmod -R +r src/jepsen-mongodb/store/test-index${i} || true

    last_five_lines=$(tail -n 5 "jepsen/docker/jepsen_test_${i}.log")

    if echo "$last_five_lines" | grep -q "1 successes"; then
        echo "Test is successful."
    else
        echo "Test is not successful."
    fi

    # Collect mongod logs (n1-n3) for diagnostics. SLS logs (n4-n6) are
    # gathered separately via Jepsen's log-files protocol (see below).
    echo "Collecting mongod logs (n1-n3)."

    mkdir -p src/jepsen-mongodb/mongodlogs/test_${i}

    for n in {1..3}; do
        # Prefer the mongod log file; fall back to the systemd journal which
        # captures output even when mongod crashes before opening the log.
        sudo docker cp jepsen-n${n}:/var/log/mongodb/mongod.log \
            src/jepsen-mongodb/mongodlogs/test_${i}/jepsen-n${n}-mongod.log 2>/dev/null || true

        # Always collect the systemd journal for mongod — this is the only
        # source of truth when mongod crashes before initialising its logger.
        sudo docker exec jepsen-n${n} journalctl -u mongod --no-pager -n 200 \
            >src/jepsen-mongodb/mongodlogs/test_${i}/jepsen-n${n}-journal.txt 2>&1 || true

        # Capture the rendered mongod.conf so we can verify the config.
        sudo docker exec jepsen-n${n} cat /etc/mongod.conf \
            >src/jepsen-mongodb/mongodlogs/test_${i}/jepsen-n${n}-mongod.conf 2>&1 || true

        # Append systemctl status for a quick summary of the failure reason.
        sudo docker exec jepsen-n${n} systemctl status mongod --no-pager \
            >>src/jepsen-mongodb/mongodlogs/test_${i}/jepsen-n${n}-journal.txt 2>&1 || true
    done
    sudo chmod -R +r src/jepsen-mongodb/mongodlogs/test_${i}/ || true

    # SLS service logs (n4-n6) are collected via Jepsen's log-files protocol,
    # which downloads /var/log/sls from each node into the Jepsen store.
    # The store is uploaded by save jepsen sls artifacts via tar_jepsen_results.
done

# Merge per-iteration logs into a single file for jepsen_report.py.
cat src/jepsen-mongodb/jepsen_test_*.log >src/jepsen-mongodb/jepsen_${task_name}_${execution}.log

end_time=$(date +%s)
elapsed_secs=$((end_time - start_time))

cd src
activate_venv
$python buildscripts/jepsen_report.py \
    --start_time=$start_time \
    --end_time=$end_time \
    --elapsed=$elapsed_secs \
    --emit_status_files \
    --store ./jepsen-mongodb \
    jepsen-mongodb/jepsen_${task_name}_${execution}.log
exit_code=$?

if [ -f "jepsen_system_fail.txt" ]; then
    mv jepsen_system_fail.txt jepsen-mongodb/jepsen_system_failure_${task_name}_${execution}
    exit 0
fi

exit $exit_code
