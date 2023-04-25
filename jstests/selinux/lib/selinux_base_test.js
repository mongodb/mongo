'use strict';

/**
 * An "abstract" base selinux test class, containing common functions that should be
 * assumed to be called by a test executor.
 *
 * Implementations for the test can extend this base clase in order to integrate
 * into evergreen/selinux_test_executor.sh
 *
 * NOTE: Implementations for this exist in both community and enterprise,
 * be cautious about modifying the base class.
 */
class SelinuxBaseTest {
    /**
     * Returns the "base" configuration per the rpm mongod.conf
     * Inheriting classes should use this base configuration and
     * extend the returned object as necessary
     */
    get config() {
        return {
            "systemLog": {
                "destination": "file",
                "logAppend": true,
                "path": "/var/log/mongodb/mongod.log",
                "verbosity": 0
            },
            "processManagement": {
                "pidFilePath": "/var/run/mongodb/mongod.pid",
                "timeZoneInfo": "/usr/share/zoneinfo"
            },
            "net": {"port": 27017, "bindIp": "127.0.0.1"},
            "storage": {"dbPath": "/var/lib/mongo"}
        };
    }

    // Notice: private definitions, e.g.: #sudo() are not
    // recognized by js linter, so leaving this declaration public
    sudo(script) {
        return run("sudo", "--non-interactive", "bash", "-c", script);
    }

    /**
     * Called by test executors (e.g. evergreen/selinux_test_executor.sh)
     * to set up the test environment
     */
    setup() {
    }

    /**
     * Called by test executors (e.g. evergreen/selinux_test_executor.sh)
     * to tear down test configurations at the end of the test run
     */
    teardown() {
    }

    /**
     * Called by test executors (e.g. evergreen/selinux_test_executor.sh)
     * to run the test. Inheriting classes must override this to run their tests
     */
    run() {
        assert("override this function");
    }
}
