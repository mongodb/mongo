
/**
 * Control the Free Monitoring Mock Webserver.
 */

// These faults must match the list of faults in mock_http_server.py, see the
// SUPPORTED_FAULT_TYPES list in mock_http_server.py
const FAULT_FAIL_REGISTER = "fail_register";
const FAULT_INVALID_REGISTER = "invalid_register";
const FAULT_HALT_METRICS_5 = "halt_metrics_5";
const FAULT_PERMANENTLY_DELETE_AFTER_3 = "permanently_delete_after_3";
const FAULT_RESEND_REGISTRATION_AT_3 = "resend_registration_at_3";
const FAULT_RESEND_REGISTRATION_ONCE = "resend_registration_once";

const DISABLE_FAULTS = "disable_faults";
const ENABLE_FAULTS = "enable_faults";

class FreeMonWebServer {
    /**
     * Create a new webserver.
     *
     * @param {string} fault_type
     * @param {bool} disableFaultsOnStartup optionally disable fault on startup
     */
    constructor(fault_type, disableFaultsOnStartup) {
        this.python = "python3";
        this.disableFaultsOnStartup = disableFaultsOnStartup || false;
        this.fault_type = fault_type;

        if (_isWindows()) {
            this.python = "python.exe";
        }

        print("Using python interpreter: " + this.python);
        this.web_server_py = "jstests/free_mon/libs/mock_http_server.py";
        this.control_py = "jstests/free_mon/libs/mock_http_control.py";

        this.pid = undefined;
        this.port = -1;
    }

    /**
     * Get the Port.
     *
     * @return {number} port number of http server
     */
    getPort() {
        return port;
    }

    /**
     * Get the URL.
     *
     * @return {string} url of http server
     */
    getURL() {
        return "http://localhost:" + this.port;
    }

    /**
     *  Start the Mock HTTP Server.
     */
    start() {
        this.port = allocatePort();
        print("Mock Web server is listening on port: " + this.port);

        let args = [this.python, "-u", this.web_server_py, "--port=" + this.port];
        if (this.fault_type) {
            args.push("--fault=" + this.fault_type);
            if (this.disableFaultsOnStartup) {
                args.push("--disable-faults");
            }
        }

        clearRawMongoProgramOutput();

        this.pid = _startMongoProgram({args: args});

        assert(checkProgram(this.pid));

        // Wait for the web server to start
        assert.soon(function() {
            return rawMongoProgramOutput().search("Mock Web Server Listening") !== -1;
        });

        print("Mock HTTP Server sucessfully started.");
    }

    /**
     *  Stop the Mock HTTP Server.
     */
    stop() {
        stopMongoProgramByPid(this.pid);
    }

    /**
     * Query the HTTP server.
     *
     * @param {string} query type
     *
     * @return {object} Object representation of JSON from the server.
     */
    query(query) {
        const out_file = "out_" + this.port + ".txt";
        const python_command = this.python + " -u " + this.control_py + " --port=" + this.port +
            " --query=" + query + " > " + out_file;

        let ret = 0;
        if (_isWindows()) {
            ret = runProgram('cmd.exe', '/c', python_command);
        } else {
            ret = runProgram('/bin/sh', '-c', python_command);
        }

        assert.eq(ret, 0);

        const result = cat(out_file);

        try {
            return JSON.parse(result);
        } catch (e) {
            jsTestLog("Failed to parse: " + result + "\n" + result);
            throw e;
        }
    }

    /**
     * Control the HTTP server.
     *
     * @param {string} query type
     */
    control(query) {
        const out_file = "out_" + this.port + ".txt";
        const python_command = this.python + " -u " + this.control_py + " --port=" + this.port +
            " --query=" + query + " > " + out_file;

        let ret = 0;
        if (_isWindows()) {
            ret = runProgram('cmd.exe', '/c', python_command);
        } else {
            ret = runProgram('/bin/sh', '-c', python_command);
        }

        assert.eq(ret, 0);
    }

    /**
     * Disable Faults
     */
    disableFaults() {
        this.control(DISABLE_FAULTS);
    }

    /**
     * Enable Faults
     */
    enableFaults() {
        this.control(ENABLE_FAULTS);
    }

    /**
     * Query the stats page for the HTTP server.
     *
     * @return {object} Object representation of JSON from the server.
     */
    queryStats() {
        return this.query("stats");
    }

    /**
     * Wait for N register calls to be received by web server.
     *
     * @throws assert.soon() exception
     */
    waitRegisters(count) {
        const qs = this.queryStats.bind(this);
        const port = this.port;
        // Wait for registration to occur
        assert.soon(function() {
            const stats = qs();
            print("w" + port + "| waiting for registers >= (" + count + ") QS : " + tojson(stats));
            return stats.registers >= count;
        }, "Failed to web server register", 60 * 1000);
    }

    /**
     * Wait for N metrics calls to be received by web server.
     *
     * @throws assert.soon() exception
     */
    waitMetrics(count) {
        const qs = this.queryStats.bind(this);
        const port = this.port;
        // Wait for metrics uploads to occur
        assert.soon(function() {
            const stats = qs();
            print("w" + port + "| waiting for metrics >= (" + count + ") QS : " + tojson(stats));
            return stats.metrics >= count;
        }, "Failed to web server metrics", 60 * 1000);
    }

    /**
     * Wait for N fault calls to e received by web server.
     *
     * @throws assert.soon() exception
     */
    waitFaults(count) {
        const qs = this.queryStats.bind(this);
        const port = this.port;
        // Wait for faults to be triggered
        assert.soon(function() {
            const stats = qs();
            print("w" + port + "| waiting for faults >= (" + count + ") QS : " + tojson(stats));
            return stats.faults >= count;
        }, "Failed to web server faults", 60 * 1000);
    }
}

/**
 * Wait for registration information to be populated in the database.
 *
 * @param {object} conn
 * @param {string} state
 */
function WaitForDiskState(conn, state) {
    'use strict';

    const admin = conn.getDB("admin");

    // Wait for registration to occur
    assert.soon(function() {
        const docs = admin.system.version.find({_id: "free_monitoring"});
        const da = docs.toArray();
        return da.length === 1 && da[0].state === state;
    }, "Failed to disk state", 60 * 1000);
}

/**
 * Wait for registration information to be populated in the database.
 *
 * @param {object} conn
 */
function WaitForRegistration(conn) {
    WaitForDiskState(conn, 'enabled');
}

/**
 * Wait for unregistration information to be populated in the database.
 *
 * @param {object} conn
 */
function WaitForUnRegistration(conn) {
    WaitForDiskState(conn, 'disabled');
}

/**
 * Get registration document.
 *
 * @param {object} registration document
 */
function FreeMonGetRegistration(conn) {
    'use strict';

    const admin = conn.getDB("admin");
    const docs = admin.system.version.find({_id: "free_monitoring"});
    const da = docs.toArray();
    return da[0];
}

/**
 * Get current Free Monitoring Status via serverStatus.
 *
 * @param {object} serverStatus.freeMonitoring section
 */
function FreeMonGetServerStatus(conn) {
    'use strict';

    const admin = conn.getDB("admin");
    return assert.commandWorked(admin.runCommand({serverStatus: 1})).freeMonitoring;
}

/**
 * Get current Free Monitoring Status via getFreeMonitoringStatus.
 *
 * @param {object} getFreeMonitoringStatus document
 */
function FreeMonGetStatus(conn) {
    'use strict';

    const admin = conn.getDB("admin");
    const reply = assert.commandWorked(admin.runCommand({getFreeMonitoringStatus: 1}));
    // FreeMonitoring has been deprecated and reports 'disabled' regardless of status.
    assert.eq(reply.state, 'disabled', 'FreeMonitoring has been deprecated');

    // Use the "true" state tucked into the 'debug' field if its available.
    return reply.debug || reply;
}

/**
 * Wait for server status state
 *
 * @param {object} conn
 * @param {string} state
 */
function WaitForFreeMonServerStatusState(conn, state) {
    'use strict';

    // Wait for registration to occur
    assert.soon(
        function() {
            let status = FreeMonGetServerStatus(conn).state;
            return status === state;
        },
        "Failed to find expected server status state: expected: '" + state +
            "', actual: " + tojson(FreeMonGetServerStatus(conn)),
        20 * 1000);
}

/**
 * Validate Free Monitoring Replica Set consistency
 * WARNING: Not valid if secondary is started with enableFreeMonitoring since it registers before it
 * joins the replica set.
 *
 * @param {object} rst
 */
function ValidateFreeMonReplicaSet(rst) {
    'use strict';

    const primary_status = FreeMonGetStatus(rst.getPrimary());
    const primary_url = primary_status.url;
    const secondary_status = FreeMonGetStatus(rst.getSecondary());
    const secondary_url = secondary_status.url;
    assert.eq(primary_url,
              secondary_url,
              `DUMP ${tojson(primary_status)} == ${tojson(secondary_status)}`);
}
