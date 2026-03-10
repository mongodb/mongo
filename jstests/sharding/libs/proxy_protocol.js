/**
 * Control the proxy protocol server.
 */

import {getPython3Binary} from "jstests/libs/python.js";

export class ProxyProtocolServer {
    /**
     * Create a new proxy protocol server.
     *
     * @param {number} ingress_port - Port to listen on for client connections.
     * @param {number} egress_port - Port to forward to (ignored when options.egressUnixSocket is set).
     * @param {number} version - Proxy protocol version (1 or 2).
     * @param {Object} [options] - Optional configuration.
     * @param {string} [options.egressUnixSocket] - Forward to this Unix domain socket instead of TCP.
     * @param {string} [options.ingressTLSCert] - PEM file for proxy's TLS server certificate + key.
     * @param {string} [options.ingressTLSCA] - CA PEM file for verifying client certs on ingress.
     */
    constructor(ingress_port, egress_port, version, options = {}) {
        this.python = getPython3Binary();

        print("Using python interpreter: " + this.python);
        this.web_server_py = "jstests/sharding/libs/proxy_protocol_server.py";

        this.pid = undefined;
        this.ingress_port = ingress_port;
        this.egress_port = egress_port;

        assert(version === 1 || version === 2);
        this.version = version;

        this.ingress_address = "127.0.0.1";
        this.egress_address = "127.0.0.1";

        // Optional: forward egress to a Unix domain socket instead of TCP.
        this.egress_unix_socket = options.egressUnixSocket || null;

        // Optional: terminate TLS on the ingress side.
        this.ingress_tls_cert = options.ingressTLSCert || null;
        this.ingress_tls_ca = options.ingressTLSCA || null;

        // The file that will serve as stdin for the ProxyProtocolServer process. This file will
        // only be used to supply tlv structs to the server.
        this.tlvFile = MongoRunner.dataPath + `proxyprotocol_tlvs_${ingress_port}_${egress_port}.json`;
        writeFile(this.tlvFile, "");
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
     * The the ingress connection string which can be passed to `new Mongo(...)`.
     */
    getIngressString() {
        return `${this.ingress_address}:${this.ingress_port}`;
    }

    /**
     * The the egress connection string pointing to the output.
     */
    getEgressString() {
        return `${this.egress_address}:${this.egress_port}`;
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
        const ingressHostPort = this.ingress_address + ":" + this.ingress_port;

        // Build the --service source (ingress) address. When TLS is configured, use the ssl://
        // scheme so the proxy-protocol library terminates TLS on the ingress side.
        let serviceIngress;
        if (this.ingress_tls_cert) {
            const params = [`cert=${this.ingress_tls_cert}`];
            if (this.ingress_tls_ca) {
                params.push(`cafile=${this.ingress_tls_ca}`);
                params.push("verify=CERT_OPTIONAL");
            }
            serviceIngress = `ssl://${ingressHostPort}?${params.join("&")}`;
        } else {
            serviceIngress = ingressHostPort;
        }

        // Build the --service destination (egress) address. When egressUnixSocket is set, the
        // actual connection goes to the Unix socket (via --unix-egress), but the library still
        // needs a destination address for CLI parsing, so we provide a dummy one.
        let serviceEgress;
        if (this.egress_unix_socket) {
            serviceEgress = `localhost:0?pp=v${this.version}`;
        } else {
            serviceEgress = this.egress_address + ":" + this.egress_port + "?pp=v" + this.version;
        }

        const args = [
            this.python,
            "-u", // unbuffered output
            this.web_server_py,
            "--pp2-tlv-file",
            this.tlvFile,
        ];

        if (this.egress_unix_socket) {
            args.push("--unix-egress", this.egress_unix_socket);
        }

        args.push("--service", serviceIngress, serviceEgress);

        clearRawMongoProgramOutput();
        this.pid = _startMongoProgram({args});

        // Wait for the web server to listen on the configured port. The log message always
        // prints the host:port regardless of whether TLS is enabled.
        let checkProgramResult, checkLogResult;
        const timeoutMillis = 30_000;
        const retryIntervalMillis = 500;
        const expectedLogPattern = `Now listening on ${ingressHostPort}`;
        assert.soon(
            () => {
                checkProgramResult = checkProgram(this.pid);
                checkLogResult = rawMongoProgramOutput(expectedLogPattern);
                return checkProgramResult["alive"] && checkLogResult !== "";
            },
            () => {
                if (!checkProgramResult["alive"]) {
                    // TODO(SERVER-110719) the fuser and ps here act as diagnostics for future cases of
                    // port collision. After more occurences of "address in use", we'll be able to
                    // figure out which program is using the same port, and this can be removed.
                    jsTestLog("Printing info from ports " + this.ingress_port);
                    const commands = [
                        ["/bin/sh", "-c", "fuser -v -n tcp " + this.ingress_port],
                        ["/bin/sh", "-c", "ps -ef | grep " + this.ingress_port],
                    ];
                    commands.map((args) => _startMongoProgram({args})).map((pid) => waitProgram(pid));
                }
                return {checkProgramResult, checkLogResult, expectedLogPattern};
            },
            timeoutMillis,
            retryIntervalMillis,
        );

        print("Proxy Protocol Server sucessfully started.");
    }

    /**
     * Update PROXY protocol v2 TLVs (type-length-value) for future connections.
     *
     * Input must be an array of objects representing the TLV. A valid array includes 0 to many TLV
     * objects with 0 or 1 SSL TLV objects.
     *
     * A TLV object has the following format:
     * - type: Number indicating the TLV type
     * - value: UTF-8 string
     *
     * An SSL TLV object has the following format:
     * - ssl: array of TLV objects
     *
     * Ex. { [{"type":0xE1,"value":"hello"}, {...}, {ssl: [{"type":0xE2,"value":"hello2"}, {...}]] }
     */
    setTLVs(tlvs) {
        const jsonString = JSON.stringify(tlvs) + "\n";
        appendFile(this.tlvFile, jsonString);
    }

    /**
     * Get the proxy server port used to connect to the egress port provided.
     * That is, return port3 in the following:
     *
     *     [client] port1 <-----> ingress_port [proxy] port3 <------> egress_port [mongo(s|d)]
     */
    getServerPort() {
        const tools = [
            {
                args: ["ss", "-nt", `dst 127.0.0.1:${this.egress_port}`],
                regex: `127.0.0.1:(\\d+)\\s+127.0.0.1:${this.egress_port}`,
            },
            {
                args: ["netstat", "-nt"],
                regex: `127.0.0.1:(\\d+)\\s+127.0.0.1:${this.egress_port}\\s*ESTABLISHED`,
            },
        ];

        for (const {args, regex} of tools) {
            clearRawMongoProgramOutput();
            const exitStatus = waitProgram(_startMongoProgram({args: args}));
            if (exitStatus !== 0) {
                print(`Attempt to use command "${args[0]}" failed with exit status ${exitStatus}`);
                continue;
            }

            const match = rawMongoProgramOutput(".*").match(regex);
            if (match === null) {
                throw Error(`The output of ${args[0]} did not contain a connection to egress port ${this.egress_port}`);
            }

            return parseInt(match[1], 10);
        }

        const commandsJson = JSON.stringify(tools.map((tool) => tool.args[0]));
        throw Error(
            `Could not find connection to egress port ${
                this.egress_port
            }: all of the following commands failed: ${commandsJson}`,
        );
    }

    /**
     * Stop the server.
     */
    stop() {
        stopMongoProgramByPid(this.pid);
        removeFile(this.tlvFile);
    }
}
