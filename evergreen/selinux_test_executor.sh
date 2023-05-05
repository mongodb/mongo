#!/bin/bash
set +o errexit

readonly k_log_path="/var/log/mongodb/mongod.log"
readonly k_mongo="$(pwd)/dist-test/bin/mongo"
readonly k_test_path="$1"
return_code=1

export PATH="$(dirname "$k_mongo"):$PATH"

function print_err() {
  echo "$@" >&2
}

function monitor_log() {
  sed "s!^!mongod| $(date '+%F %H-%M-%S') !" <(sudo --non-interactive tail -f $k_log_path)
}

function output_ausearch() {
  local cmd_parameters="AVC,USER_AVC,SELINUX_ERR,USER_SELINUX_ERR"

  echo ""
  echo "====== SELinux errors (ausearch -m $cmd_parameters): ======"
  sudo --non-interactive ausearch -m $cmd_parameters -ts $1
}

function output_journalctl() {
  echo ""
  echo "============================== journalctl ========================================="
  sudo --non-interactive journalctl --no-pager --catalog --since="$1" | grep -i mongo
}

function fail_and_exit_err() {

  echo ""
  echo "==================================================================================="
  echo "++++++++ Test failed, outputting last 5 seconds of additional log info ++++++++++++"
  echo "==================================================================================="
  output_ausearch "$(date --utc --date='5 seconds ago' '+%x %H:%M:%S')"
  output_journalctl "$(date --utc --date='5 seconds ago' +'%Y-%m-%d %H:%M:%S')"

  echo ""
  echo "==== FAIL: $1 ===="
  exit 1
}

function create_mongo_config() {
  echo "Writing /etc/mongod.conf for $k_test_path:"
  "$k_mongo" --nodb --norc --quiet --eval='
        assert(load("'"$k_test_path"'"));
        const test = new TestDefinition();
        print(JSON.stringify(test.config, null, 2));

    ' | sudo --non-interactive tee /etc/mongod.conf
}

function start_mongod() {
  # Start mongod and if it won't come up, fail and exit

  sudo --non-interactive systemctl start mongod \
    && sudo --non-interactive systemctl status mongod || (
    fail_and_exit_err "systemd failed to start mongod server!"
  )
}

function wait_for_mongod_to_accept_connections() {
  # Once the mongod process starts via systemd, it can still take a couple of seconds
  # to set up and accept connections... we will wait for log id 23016 to show up
  # indicating that the server is ready to accept incoming connections before starting the tests

  local server_ready=0
  local wait_seconds=2
  local wait_retries_max=30
  local wait_retries=0

  while [[ $wait_retries -le $wait_retries_max ]]; do
    local server_status="$(grep 23016 $k_log_path || echo "")"

    if [ "$server_status" != "" ]; then
      server_ready=1
      break
    fi

    sleep $wait_seconds
    ((wait_retries++))
  done

  if [ ! $server_ready ]; then
    fail_and_exit_err "failed to connect to mongod server after waiting for $(($wait_seconds * $wait_retries)) seconds!"
  fi
}

function clear_mongo_config() {
  # stop mongod, zero mongo log, clean up database, set all booleans to off
  sudo --non-interactive bash -c '
        systemctl stop mongod

        rm -f '"$k_log_path"'
        touch '"$k_log_path"'
        chown mongod '"$k_log_path"'

        rm -rf /var/lib/mongo/*

        rm -rf /etc/sysconfig/mongod /etc/mongod

        setsebool mongod_can_connect_ldap off
        setsebool mongod_can_use_kerberos off
    '
}

function exit_with_code() {
  exit $return_code
}

function setup_test_definition() {
  "$k_mongo" --nodb --norc --quiet --eval='
        assert(load("'"$k_test_path"'"));
        (() => { 
            const test = new TestDefinition();
            print("Running setup() for '"$k_test_path"'");
            test.setup();
        })();
    '
}

function run_test() {
  "$k_mongo" --norc --gssapiServiceName=mockservice --eval='
        assert(load("'"$k_test_path"'"));
        print("Running test '"$k_test_path"'");

	(() => {
            const test = new TestDefinition();

            try {
                test.run();
            } finally {
                test.teardown();
            }
        })();
    ' || fail_and_exit_err "Test failed"

  echo "SUCCESS: $k_test_path"
}

if [ ! -f "$k_mongo" ]; then
  print_err "Mongo shell at $k_mongo is missing"
  exit 1
fi

if [ ! -f "$k_test_path" ]; then
  print_err "No test supplied or test file not found. Run:"
  print_err "$(basename "${BASH_SOURCE[0]}") <path>"
  exit 1
fi

# Ensure file containing tests is valid before executing
if ! "$k_mongo" --nodb --norc --quiet "$k_test_path"; then
  print_err "File $k_test_path has syntax errors"
  exit 1
fi

echo "STARTING TEST: $k_test_path"

clear_mongo_config
create_mongo_config
setup_test_definition

# start log monitor, also kill it on exit
monitor_log &
monitor_pid="$!"
trap "sudo --non-interactive pkill -P $monitor_pid; exit_with_code" SIGINT SIGTERM ERR EXIT

start_mongod
wait_for_mongod_to_accept_connections
run_test

return_code=0
