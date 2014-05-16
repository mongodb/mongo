#!/bin/sh -e

start() {
    mkdir _testdb
    cd _testdb
    mkdir db1 db2 rs1a rs1b rs1c rs2a rs2b rs2c rs3a rs3b rs3c rs4a cfg1 cfg2 cfg3
    ln -s ../testdb/supervisord.conf supervisord.conf
    echo keyfile > keyfile
    chmod 600 keyfile
    echo "Running supervisord..."
    supervisord || ( echo "Supervisord failed executing ($?)" && exit 1 )
    COUNT=$(grep '^\[program' supervisord.conf | wc -l)
    echo "Supervisord is up, starting $COUNT processes..."
    for i in $(seq 10); do
        RUNNING=$(supervisorctl status | grep RUNNING | wc -l)
        echo "$RUNNING processes running..."
        if [ x$COUNT = x$RUNNING ]; then
            echo "Running setup.js with mongo..."
            mongo --nodb ../testdb/init.js
            exit 0
        fi
        sleep 1
    done
    echo "Failed to start all processes. Check out what's up at $PWD now!"
    exit 1
}

stop() {
    if [ -d _testdb ]; then
        echo "Shutting down test cluster..."
        (cd _testdb && supervisorctl shutdown)
        rm -rf _testdb
    fi
}


if [ ! -f suite_test.go ]; then
    echo "This script must be run from within the source directory."
    exit 1
fi

case "$1" in

    start)
        start
        ;;

    stop)
        stop
        ;;

esac

# vim:ts=4:sw=4:et
