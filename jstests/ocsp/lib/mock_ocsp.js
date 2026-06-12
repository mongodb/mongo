/**
 * Starts a mock OCSP Server to test
 * OCSP certificate revocation.
 */
load("jstests/ocsp/lib/ocsp_helpers.js");
load("jstests/libs/python.js");

// These are a list of faults to match the list of faults
// in ocsp_mock.py.
const FAULT_REVOKED = "revoked";
const FAULT_UNKNOWN = "unknown";

const OCSP_PROGRAM = "jstests/ocsp/lib/ocsp_mock.py";

// Standalone TLS server that staples a malformed (undecodable) OCSP response. Launched by
// MockMalformedStapleServer.
const MALFORMED_STAPLE_SERVER_PROGRAM = "jstests/ocsp/lib/malformed_staple_server.py";

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
                response_delay_secs = 0,
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
        this.response_delay_secs = response_delay_secs;
        this.include_extraneous_status = include_extraneous_status;
        this.issuer_hash_algorithm = issuer_hash_algorithm;
    }

    start() {
        print("Mock OCSP Server will listen on port: " + this.port);
        let args = [
            this.python,
            "-u",
            OCSP_PROGRAM,
            "--port=" + this.port,
            "--ca_file=" + this.ca_file,
            "--ocsp_responder_cert=" + this.ocsp_cert_file,
            "--ocsp_responder_key=" + this.ocsp_cert_key,
            "--verbose",
        ];

        if (this.fault_type) {
            args.push("--fault=" + this.fault_type);
        }

        if (this.next_update_secs || this.next_update_secs === 0) {
            args.push("--next_update_seconds=" + this.next_update_secs);
        }

        if (this.response_delay_secs) {
            args.push("--response_delay_seconds=" + this.response_delay_secs);
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
        if (!this.pid) {
            print("Not stopping Mock OCSP Server, it was never started");
            return;
        }

        print("Stopping Mock OCSP Server");

        if (_isWindows()) {
            // we use taskkill because we need to kill children
            waitProgram(_startMongoProgram("taskkill", "/F", "/T", "/PID", this.pid));
            // waitProgram to ignore error code
            waitProgram(this.pid);
        } else {
            const kSIGINT = 2;
            stopMongoProgramByPid(this.pid, kSIGINT);
        }

        print("Mock OCSP Server stop complete");
    }
}

/**
 * A mock TLS server that staples a malformed (single 0x00 byte) OCSP response
 * during the handshake. It launches malformed_staple_server.py to exercise a TLS
 * client's handling of an undecodable stapled OCSP response.
 */
class MockMalformedStapleServer {
    /**
     * Create a new malformed-staple TLS server.
     *
     * @param {number} port - port to listen on.
     * @param {string} tls_cert_key_file - PEM file with the certificate chain and
     *     private key the server presents. Must carry an OCSP AIA URI so that the
     *     client invokes its stapled-response handling.
     */
    constructor(port, tls_cert_key_file = OCSP_SERVER_CERT) {
        this.python = "python3";
        if (_isWindows()) {
            this.python = "python.exe";
        }
        this.port = port;
        this.tls_cert_key_file = tls_cert_key_file;
    }

    start() {
        print("Mock malformed staple TLS server will listen on port: " + this.port);
        const args = [
            this.python,
            "-u",
            MALFORMED_STAPLE_SERVER_PROGRAM,
            "--port=" + this.port,
            "--tls_cert_key_file=" + this.tls_cert_key_file,
            "--verbose",
        ];

        clearRawMongoProgramOutput();
        const pid = _startMongoProgram({args: args});
        assert(checkProgram(pid).alive);

        assert.soon(function() {
            const output = rawMongoProgramOutput(".*");
            assert.eq(
                output.search("Address already in use"), -1, "Mock staple server port in use");
            return output.search("Malformed staple TLS server listening on") !== -1;
        }, "Mock malformed staple TLS server failed to start");

        this.pid = pid;
    }

    /**
     * Get the host:port the server is listening on.
     *
     * @return {string} host of the TLS server.
     */
    getHost() {
        return "localhost:" + this.port;
    }

    /**
     * Stop the TLS server. If SIGINT doesn't kill it after a few seconds, kill with SIGKILL.
     */
    stop() {
        if (!this.pid) {
            print("Not stopping Mock malformed staple TLS server, it was never started");
            return;
        }

        print("Stopping Mock malformed staple TLS server");
        const kSIGINT = 2;
        stopMongoProgramByPid(this.pid, kSIGINT);

        for (let i = 0; i < 50; i++) {
            if (!checkProgram(this.pid).alive) {
                print("Mock malformed staple TLS server stop complete");
                return;
            }
            sleep(100);
        }

        const kSIGKILL = 9;
        stopMongoProgramByPid(this.pid, kSIGKILL);
        print("Mock malformed staple TLS server stop complete");
    }
}
