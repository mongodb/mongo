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

class ResponderCertSet {
    /**
     * Set of certificates for the OCSP responder.'
     * @param {string} cafile
     * @param {string} certfile
     * @param {string} keyfile
     */
    constructor(cafile, certfile, keyfile) {
        this.cafile = cafile;
        this.certfile = certfile;
        this.keyfile = keyfile;
    }
}

const OCSP_DELEGATE_RESPONDER =
    new ResponderCertSet(OCSP_CA_PEM, OCSP_RESPONDER_CERT, OCSP_RESPONDER_KEY);
const OCSP_CA_RESPONDER = new ResponderCertSet(OCSP_CA_PEM, OCSP_CA_CERT, OCSP_CA_KEY);
const OCSP_INTERMEDIATE_RESPONDER = new ResponderCertSet(OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM,
                                                         OCSP_INTERMEDIATE_CA_ONLY_CERT,
                                                         OCSP_INTERMEDIATE_CA_ONLY_KEY);

class MockOCSPServer {
    /**
     * Create a new OCSP Server.
     *
     * @param {string} fault_type
     * @param {number} next_update_secs
     * @param {object} responder_certificate_set
     */
    constructor(fault_type,
                next_update_secs,
                responder_certificate_set = OCSP_DELEGATE_RESPONDER,
                include_extraneous_status = false,
                issuer_hash_algorithm = "") {
        this.python = "python3";
        this.fault_type = fault_type;

        if (_isWindows()) {
            this.python = "python.exe";
        }

        this.ca_file = responder_certificate_set.cafile;
        this.ocsp_cert_file = responder_certificate_set.certfile;
        this.ocsp_cert_key = responder_certificate_set.keyfile;

        print("Using python interpreter: " + this.python);
        // The port must be hard coded to match the port of the
        // responder in the certificates.
        this.port = 8100;
        this.next_update_secs = next_update_secs;
        this.include_extraneous_status = include_extraneous_status;
        this.issuer_hash_algorithm = issuer_hash_algorithm;
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

        if (this.include_extraneous_status) {
            args.push("--include_extraneous_status");
        }

        if (this.issuer_hash_algorithm) {
            args.push("--issuer_hash_algorithm=" + this.issuer_hash_algorithm);
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