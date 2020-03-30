/**
 * Starts a mock OCSP Server to test
 * OCSP certificate revocation.
 */
load("jstests/ocsp/lib/ocsp_helpers.js");

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
     * @param {number} next_update_secs
     * @param {boolean} responder_is_ca
     */
    constructor(fault_type, next_update_secs, responder_is_ca = false) {
        this.python = "python3";
        this.fault_type = fault_type;

        if (_isWindows()) {
            this.python = "python.exe";
        }

        if (responder_is_ca) {
            this.ocsp_cert_file = OCSP_CA_CERT;
            this.ocsp_cert_key = OCSP_CA_KEY;
        } else {
            this.ocsp_cert_file = OCSP_RESPONDER_CERT;
            this.ocsp_cert_key = OCSP_RESPONDER_KEY;
        }

        print("Using python interpreter: " + this.python);
        this.ca_file = OCSP_CA_PEM;
        // The port must be hard coded to match the port of the
        // responder in the certificates.
        this.port = 8100;
        this.next_update_secs = next_update_secs;
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

        if (this.fault_type) {
            args.push("--fault=" + this.fault_type);
        }

        if (this.next_update_secs) {
            args.push("--next_update_seconds=" + this.next_update_secs);
        }

        clearRawMongoProgramOutput();

        this.pid = _startMongoProgram({args: args});
        assert(checkProgram(this.pid).alive);

        assert.soon(function() {
            // Change this line if the OCSP endpoint changes
            return rawMongoProgramOutput().search("Running on http://127.0.0.1:8100/") !== -1;
        });

        sleep(2000);
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