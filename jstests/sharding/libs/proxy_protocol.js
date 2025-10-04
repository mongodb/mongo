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

        this.ingress_address = "127.0.0.1";
        this.egress_address = "127.0.0.1";
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
        const ingressInterface = this.ingress_address + ":" + this.ingress_port;

        const args = [
            this.python,
            "-u", // unbuffered output
            this.web_server_py,
            "--service",
            ingressInterface,
            this.egress_address + ":" + this.egress_port + "?pp=v" + this.version,
        ];

        clearRawMongoProgramOutput();
        this.pid = _startMongoProgram({args});

        // Wait for the web server to listen on the configured port.
        let checkProgramResult, checkLogResult;
        const timeoutMillis = 30_000;
        const retryIntervalMillis = 500;
        const expectedLogPattern = `Now listening on ${ingressInterface}`;
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
    }
}
