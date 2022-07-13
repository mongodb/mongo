#!/bin/bash

set -o errexit
set -o xtrace

mongo="$(pwd)/dist-test/bin/mongo"
export PATH="$(dirname "$mongo"):$PATH"
if [ ! -f "$mongo" ]; then
  echo "Mongo shell at $mongo is missing"
  exit 1
fi

function print() {
  echo "$@" >&2
}

function monitor_log() {
  sed "s!^!mongod| $(date '+%F %H-%M-%S') !" <(sudo --non-interactive tail -f /var/log/mongodb/mongod.log)
}

TEST_PATH="$1"
if [ ! -f "$TEST_PATH" ]; then
  print "No test supplied or test file not found. Run:"
  print "  $(basename "${BASH_SOURCE[0]}") <path>"
  exit 1
fi

# test file is even good before going on
if ! "$mongo" --nodb --norc --quiet "$TEST_PATH"; then
  print "File $TEST_PATH has syntax errors"
  exit 1
fi

# stop mongod, zero mongo log, clean up database, set all booleans to off
sudo --non-interactive bash -c '
    systemctl stop mongod

    rm -f /var/log/mongodb/mongod.log
    touch /var/log/mongodb/mongod.log
    chown mongod /var/log/mongodb/mongod.log

    rm -rf /var/lib/mongo/*

    rm -rf /etc/sysconfig/mongod /etc/mongod

    setsebool mongod_can_connect_snmp off
    setsebool mongod_can_connect_ldap off
    setsebool mongod_can_use_kerberos off
'

# create mongo config
"$mongo" --nodb --norc --quiet --eval='
    assert(load("'"$TEST_PATH"'"));
    const test = new TestDefinition();
    print(typeof(test.config) === "string" ? test.config : JSON.stringify(test.config, null, 2));
' | sudo --non-interactive tee /etc/mongod.conf

# setup
"$mongo" --nodb --norc --quiet --eval='
    assert(load("'"$TEST_PATH"'"));
    const test = new TestDefinition();
    jsTest.log("Running setup()");
    test.setup();
'

# start log monitor, also kill it on exit
monitor_log &
MONITORPID="$!"
trap "sudo --non-interactive pkill -P $MONITORPID" SIGINT SIGTERM ERR EXIT

# start mongod and if it won't come up, log SELinux errors
ts="$(date --utc --date='1 seconds ago' '+%x %H:%M:%S')"
tsj="$(date --utc --date='1 seconds ago' +'%Y-%m-%d %H:%M:%S')"
sudo --non-interactive systemctl start mongod \
  && sudo --non-interactive systemctl status mongod || (
  set +o errexit
  echo "================== SELinux errors: =================="
  sudo --non-interactive ausearch -m AVC,USER_AVC,SELINUX_ERR,USER_SELINUX_ERR -ts $ts
  echo "================== journalctl =================="
  sudo --non-interactive journalctl --no-pager --catalog --since="$tsj" | grep -i mongo
  echo "================== /var/log/mongodb/mongod.log =================="
  sudo --non-interactive cat /var/log/mongodb/mongod.log
  echo "==== FAIL: mongod service was not started successfully"
  exit 1
)

# run test and teardown
"$mongo" --norc --gssapiServiceName=mockservice --eval='
    assert(load("'"$TEST_PATH"'"));
    // name is such to prevent collisions
    const test_812de7ce = new TestDefinition();
    try {
        jsTest.log("Running test");
        test_812de7ce.run();
    } finally {
        test_812de7ce.teardown();
    }
' || (
  echo "==== FAIL: test returned result: $?"
  echo "=== SELinux errors:"
  set +o errexit
  sudo --non-interactive ausearch -m AVC,USER_AVC,SELINUX_ERR,USER_SELINUX_ERR -ts $ts
  echo "=== /var/log/mongodb/mongod.log:"
  sudo --non-interactive cat /var/log/mongodb/mongod.log
  exit 1
)

set +o xtrace
echo "SUCCESS: $TEST_PATH"
