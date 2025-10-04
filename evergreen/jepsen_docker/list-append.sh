DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

# this file does not use set -euo pipefail because we determine test success or
# failure parsing the log file, instead of the return value.
# This whole script must run to ensure the report is generated and test
# artifacts are placed in the right location.

# actually run the tests
start_time=$(date +%s)

# Previously, we use the "--test-count 30" option to repeat the Jepsen test 30 times.
# However, to ensure the preservation of all the mongod and mongos logs for each failed test,
# we have implemented a for loop to iterate the test 30 times.
for i in {1..30}; do
    cd jepsen/docker

    sudo docker exec jepsen-control bash --login -c "\
  cd /jepsen/mongodb && \
  lein run test-all -w list-append \
  -n n1 -n n2 -n n3 -n n4 -n n5 -n n6 -n n7 -n n8 -n n9 \
  -r 1000 \
  --concurrency 3n \
  --time-limit 120 \
  --max-writes-per-key 128 \
  --read-concern majority \
  --write-concern majority \
  --txn-read-concern snapshot \
  --txn-write-concern majority \
  --nemesis-interval 1 \
  --nemesis partition \
  --test-count 1" | tee jepsen_test_${i}.log

    cd ../../

    # copy files to expected locations for archiving
    mkdir -p src/jepsen-mongodb/store/test-index${i}
    sudo docker cp jepsen-control:/jepsen/mongodb/store src/jepsen-mongodb/store/test-index${i}
    cp jepsen/docker/jepsen_test_${i}.log src/jepsen-mongodb/
    sudo docker cp jepsen-control:/jepsen/mongodb src/jepsen-workdir

    # Get the last five lines of the log file like below example
    # 1 successes
    # 0 unknown
    # 0 crashed
    # 0 failures
    #
    last_five_lines=$(tail -n 5 "jepsen/docker/jepsen_test_${i}.log")

    # Check if the "1 successes" string is in the last five lines
    if echo "$last_five_lines" | grep -q "1 successes"; then
        echo "Test is successful, no additional logs will be spared."
    else
        echo "Test is not successful. Sparing mongod and mongos logs into 'src/jepsen-mongodb/mongodlogs/' and 'src/jepsen-mongodb/mongoslogs/' directories."

        # copy mongod logs
        mkdir -p src/jepsen-mongodb/mongodlogs/test_${i}
        # loop 9 docker containers
        for n in {1..9}; do
            sudo docker cp jepsen-n${n}:/var/log/mongodb/mongod.log src/jepsen-mongodb/mongodlogs/test_${i}/jepson-n${n}-mongod.log
        done
        sudo chmod +r src/jepsen-mongodb/mongodlogs/test_${i}/*.log

        # copy mongos logs
        mkdir -p src/jepsen-mongodb/mongoslogs/test_${i}
        for n in {1..9}; do
            sudo docker cp jepsen-n${n}:/var/log/mongodb/mongos.stdout src/jepsen-mongodb/mongoslogs/test_${i}/jepson-n${n}-mongos.stdout
        done
        sudo chmod +r src/jepsen-mongodb/mongoslogs/test_${i}/*.stdout
    fi
done

# Merge all jepsen_test_${i}.log into a single file
cat src/jepsen-mongodb/jepsen_test_*.log >src/jepsen-mongodb/jepsen_${task_name}_${execution}.log

end_time=$(date +%s)
elapsed_secs=$((end_time - start_time))

cd src
activate_venv
$python buildscripts/jepsen_report.py --start_time=$start_time --end_time=$end_time --elapsed=$elapsed_secs --emit_status_files --store ./jepsen-mongodb jepsen-mongodb/jepsen_${task_name}_${execution}.log
exit_code=$?

if [ -f "jepsen_system_fail.txt" ]; then
    mv jepsen_system_fail.txt jepsen-mongodb/jepsen_system_failure_${task_name}_${execution}
    exit 0
fi

exit $exit_code
