/**
 * Starts a mock OCSP Server to test
 * OCSP certificate revocation.
 */

// These are a list of faults to match the list of faults
// in ocsp_mock.py.
const FAULT_REVOKED = "revoked";
const FAULT_UNKNOWN = "unknown";

const OCSP_PROGRAM = "jstests/ocsp/lib/ocsp_mock.py";

class MockOCSPServer {
    /**
     * Create a new OCSP Server.
     *
     * @param {string} fault_type
     */
    constructor(fault_type) {
        this.python = "python3";
        this.fault_type = fault_type;

        if (_isWindows()) {
            this.python = "python.exe";
        }

        print("Using python interpreter: " + this.python);
        this.ca_file = "jstests/libs/ocsp/ca.crt";
        this.ocsp_cert_file = "jstests/libs/ocsp/ocsp_cert.crt";
        this.ocsp_cert_key = "jstests/libs/ocsp/ocsp_cert.key";
        // The port must be hard coded to match the port of the
        // responder in the certificates.
        this.port = 8100;
    }

    start() {
        print("Mock OCSP Server will listen on port: " + this.port);
        let args = [
            this.python,
            "-u",
            OCSP_PROGRAM,
            "-p=" + this.port,
            "--ca_file=" + this.ca_file,
            "--ocsp_responder_cert=" + this.ocsp_cert_file,
            "--ocsp_responder_key=" + this.ocsp_cert_key
        ];

        this.pid = _startMongoProgram({args: args});
        assert(checkProgram(this.pid).alive);

        if (this.fault_type) {
            args.push("--fault=" + this.fault_type);
        }

        assert.soon(function() {
            return rawMongoProgramOutput().search("Mock OCSP Responder is running") !== -1;
        });

        sleep(1000);
        print("Mock OCSP Server successfully started");
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
     * Stop the web server
     */
    stop() {
        stopMongoProgramByPid(this.pid);
    }
}