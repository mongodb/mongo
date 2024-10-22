/**
 * Control the proxy protocol server.
 */

import {getPython3Binary} from "jstests/libs/python.js";

export class ProxyProtocolServer {
    /**
     * Create a new proxy protocol server.
     */
    constructor(ingress_port, egress_port, version) {
        this.python = getPython3Binary();

        print("Using python interpreter: " + this.python);
        this.web_server_py = "jstests/sharding/libs/proxy_protocol_server.py";

        this.pid = undefined;
        this.ingress_port = ingress_port;
        this.egress_port = egress_port;

        assert(version === 1 || version === 2);
        this.version = version;
    }

    /**
     * Get the ingress port - the port over which a client wishing to appear proxied should connect.
     *
     * @return {number} ingress port number
     */
    getIngressPort() {
        return this.ingress_port;
    }

    /**
     * Get the egress port - the port that mongos should be listening on for proxied connections.
     *
     * @return {number} egress port number
     */
    getEgressPort() {
        return this.egress_port;
    }

    /**
     * Start the server.
     */
    start() {
        print("Proxy protocol server is listening on port: " + this.ingress_port);
        print("Proxy protocol server is proxying to port: " + this.egress_port);

        let args = [
            this.python,
            "-u",
            this.web_server_py,
            "--service",
            "127.0.0.1:" + this.ingress_port,
            "127.0.0.1:" + this.egress_port + "?pp=v" + this.version
        ];

        clearRawMongoProgramOutput();

        this.pid = _startMongoProgram({args: args});
        // We assume proxyprotocol.create_server has run (and possibly error'd) before the 3
        // second sleep finishes. When `checkProgram` asserts true, we assume that the
        // proxyprotocol server is up and running.
        sleep(3000);
        if (!checkProgram(this.pid)["alive"]) {
            // TODO the fuser and ps here act as diagnostics for future cases of port collision.
            // After more occurences of "address in use", we'll be able to figure out which
            // program is using the same port, and this can be removed.
            jsTestLog("Printing info from ports " + this.ingress_port);
            let fuserArgs = ["/bin/sh", "-c", "fuser -v -n tcp " + this.ingress_port];
            let psArgs = ["/bin/sh", "-c", "ps -ef | grep " + this.ingress_port];
            _startMongoProgram({args: fuserArgs});
            _startMongoProgram({args: psArgs});
            // Give time for fuser to finish running.
            sleep(3000);
            assert(false, "Failed to create a ProxyProtocolServer.");
        }

        // Wait for the web server to start
        assert.soon(function() {
            return rawMongoProgramOutput(".*").search("Starting proxy protocol server...") !== -1;
        });

        print("Proxy Protocol Server sucessfully started.");
    }

    /**
     * Stop the server.
     */
    stop() {
        stopMongoProgramByPid(this.pid);
    }
}
