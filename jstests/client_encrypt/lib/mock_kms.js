/**
 * Starts a mock KMS Server to test
 * FLE encryption and decryption.
 */

// These faults must match the list of faults in kms_http_server.py, see the
// SUPPORTED_FAULT_TYPES list in kms_http_server.py
const FAULT_ENCRYPT = "fault_encrypt";
const FAULT_ENCRYPT_CORRECT_FORMAT = "fault_encrypt_correct_format";
const FAULT_ENCRYPT_WRONG_FIELDS = "fault_encrypt_wrong_fields";
const FAULT_ENCRYPT_BAD_BASE64 = "fault_encrypt_bad_base64";
const FAULT_DECRYPT = "fault_decrypt";
const FAULT_DECRYPT_CORRECT_FORMAT = "fault_decrypt_correct_format";
const FAULT_DECRYPT_WRONG_KEY = "fault_decrypt_wrong_key";

const DISABLE_FAULTS = "disable_faults";
const ENABLE_FAULTS = "enable_faults";

class MockKMSServer {
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

        this.ca_file = "jstests/libs/ca.pem";
        this.server_cert_file = "jstests/libs/server.pem";
        this.web_server_py = "jstests/client_encrypt/lib/kms_http_server.py";
        this.control_py = "jstests/client_encrypt/lib/kms_http_control.py";
        this.port = -1;
    }

    /**
     * Start a web server
     */
    start() {
        this.port = allocatePort();
        print("Mock Web server is listening on port: " + this.port);

        let args = [
            this.python,
            "-u",
            this.web_server_py,
            "--port=" + this.port,
            "--ca_file=" + this.ca_file,
            "--cert_file=" + this.server_cert_file
        ];
        if (this.fault_type) {
            args.push("--fault=" + this.fault_type);
            if (this.disableFaultsOnStartup) {
                args.push("--disable-faults");
            }
        }

        this.pid = _startMongoProgram({args: args});
        assert(checkProgram(this.pid));

        assert.soon(function() {
            return rawMongoProgramOutput().search("Mock KMS Web Server Listening") !== -1;
        });
        sleep(1000);
        print("Mock KMS Server successfully started");
    }

    _runCommand(cmd) {
        let ret = 0;
        if (_isWindows()) {
            ret = runProgram('cmd.exe', '/c', cmd);
        } else {
            ret = runProgram('/bin/sh', '-c', cmd);
        }

        assert.eq(ret, 0);
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
            " --ca_file=" + this.ca_file + " --query=" + query + " > " + out_file;

        this._runCommand(python_command);

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
        const python_command = this.python + " -u " + this.control_py + " --port=" + this.port +
            " --ca_file=" + this.ca_file + " --query=" + query;

        this._runCommand(python_command);
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
     * Get the URL.
     *
     * @return {string} url of http server
     */
    getURL() {
        return "https://localhost:" + this.port;
    }

    /**
     * Stop the web server
     */
    stop() {
        stopMongoProgramByPid(this.pid);
    }
}
