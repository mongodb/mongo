/**
 * MongoDB Shell JS Debug Session
 *
 * Implements the Debug Adapter Protocol for debugging JavaScript in MongoDB Shell.
 *   https://microsoft.github.io/debug-adapter-protocol/overview
 */

const {
    Breakpoint,
    BreakpointEvent,
    DebugSession,
    InitializedEvent,
    OutputEvent,
    StoppedEvent,
    // https://github.com/microsoft/vscode-debugadapter-node/tree/main/adapter
} = require("vscode-debugadapter");
const net = require("net");

// ID of the associated thread in the debug protocol.
// This is single-threaded here, so it is a constant 1.
const THREAD_ID = 1;

class MongoShellDebugSession extends DebugSession {
    constructor() {
        super();
        this.setDebuggerLinesStartAt1(true);
        this.setDebuggerColumnsStartAt1(true);

        // State
        this.debugConnection = null;
        this.debugServer = null;
        this.connected = false;

        // Handles for variables
        this.breakpoints = new Map();

        // Request tracking
        this.messageSeq = 1;
        this.pendingRequests = new Map();
    }

    // Initialize - tell VSCode what we support
    initializeRequest(response, _args) {
        response.body = {
            supportsConfigurationDoneRequest: true,
            supportsEvaluateForHovers: true,
            supportsSetVariable: true,
            supportsConditionalBreakpoints: true,
            supportsLogPoints: true,
            supportsBreakpointLocationsRequest: true,
            supportsTerminateRequest: true,
        };
        this.sendResponse(response);
        this.sendEvent(new InitializedEvent());
    }

    // Attach - start a debug server, listening for a mongo shell to attach to
    async attachRequest(response, args) {
        try {
            await this.startDebugServer(args.debugPort || 9229);
            this.log(`Waiting for mongo shell to connect on port ${args.debugPort || 9229}...`);
            this.log("Use resmoke's --shellJSDebugMode flag when running a JS test file to stop on breakpoints.");
            this.sendResponse(response);
        } catch (err) {
            this.sendErrorResponse(response, 1000, `Failed to attach: ${err.message}`);
        }
    }

    // Start TCP server to accept connections from mongo shell
    startDebugServer(port) {
        return new Promise((resolve, reject) => {
            this.debugServer = net.createServer((socket) => {
                this.debugConnection = socket;
                this.connected = true;
                this.setupDebugConnection();
            });

            this.debugServer.listen(port, "localhost", () => {
                this.log(`Debug server listening on port ${port}`);
                resolve();
            });

            this.debugServer.on("error", reject);
        });
    }

    // Set up message handling for debug protocol
    setupDebugConnection() {
        this.debugConnection.on("data", (data) => {
            let line = data.toString();
            if (!line.trim()) return;

            try {
                const msg = JSON.parse(line);
                this.handleDebugMessage(msg);
            } catch (err) {
                this.log(`Failed to parse: ${line}`, "stderr");
                this.log(err);
            }
        });

        this.debugConnection.on("end", () => {
            this.connected = false;
        });

        // Send any queued breakpoints from before connection (attach mode)
        this.sendQueuedBreakpoints();
    }

    // Helper to send output to debug console
    log(text, category = "stdout") {
        this.sendEvent(new OutputEvent(text + "\n", category));
    }

    disconnectRequest(response, _args) {
        this.cleanup();
        this.sendResponse(response);
    }

    // Clean up resources
    cleanup() {
        if (this.debugConnection) {
            this.debugConnection.end();
            this.debugConnection = null;
        }

        if (this.debugServer) {
            this.debugServer.close();
            this.debugServer = null;
        }

        this.connected = false;
    }

    // Set breakpoints
    setBreakPointsRequest(response, args) {
        let filePath = args.source.path;
        const mongoRepo = process.env["MONGO_REPO"];
        if (mongoRepo && filePath.startsWith(mongoRepo)) {
            // Trim the MONGO_REPO prefix and any leading slash
            filePath = filePath.substring(mongoRepo.length).replace(/^\/+/, "");
        }

        const lines = (args.breakpoints || []).map((bp) => ({
            line: bp.line,
            condition: bp.condition,
            logMessage: bp.logMessage,
        }));

        // If not connected yet (attach mode), return unverified breakpoints
        // They will be sent to the shell once it connects
        if (!this.connected) {
            const bps = lines.map((bp) => {
                const breakpoint = new Breakpoint(false, bp.line, 0);
                return breakpoint;
            });

            this.breakpoints.set(filePath, {lines, unverified: bps});
            response.body = {breakpoints: bps};
            this.sendResponse(response);
            return;
        }

        // Connected - send to shell
        this.sendCommand("setBreakpoints", {source: filePath, lines})
            .then((result) => {
                const bps = result.breakpoints.map((bp) => {
                    const breakpoint = new Breakpoint(bp.verified, bp.line, bp.column);
                    breakpoint.id = bp.id;
                    return breakpoint;
                });

                this.breakpoints.set(filePath, bps);
                response.body = {breakpoints: bps};
                this.sendResponse(response);
            })
            .catch((err) => {
                this.sendErrorResponse(response, 1002, `Failed to set breakpoints: ${err.message}`);
            });
    }

    // Send breakpoints that were set before shell connected
    sendQueuedBreakpoints() {
        for (const [filePath, value] of this.breakpoints.entries()) {
            // Check if these are unverified (queued) breakpoints
            if (value.lines && value.unverified) {
                this.log(
                    `Sending queued breakpoints for ${filePath}, lines ${value.lines.map((l) => l.line).join(", ")}`,
                );
                this.sendCommand("setBreakpoints", {source: filePath, lines: value.lines})
                    .then((result) => {
                        const bps = result.breakpoints.map((bp) => {
                            const breakpoint = new Breakpoint(bp.verified, bp.line, bp.column);
                            breakpoint.id = bp.id;
                            return breakpoint;
                        });

                        // Update stored breakpoints
                        this.breakpoints.set(filePath, bps);

                        // Send breakpoint changed events for each one
                        bps.forEach((bp) => {
                            this.sendEvent(new BreakpointEvent("changed", bp));
                        });
                    })
                    .catch((err) => {
                        this.log(`Failed to set queued breakpoints: ${err.message}`, "stderr");
                    });
            }
        }
    }
    // Handle incoming messages from debug server
    handleDebugMessage(msg) {
        if (msg.type === "event") {
            this.handleEvent(msg);
        } else if (msg.type === "response" && msg.seq) {
            const resolver = this.pendingRequests.get(msg.seq);
            if (resolver) {
                this.log(`Resolving pending request for seq ${msg.seq}`);
                resolver(msg.body);
                this.pendingRequests.delete(msg.seq);
            }
        }
    }

    handleEvent(msg) {
        switch (msg.event) {
            case "output":
                this.log(msg.body.output, msg.body.category);
                break;
            case "breakpoint": {
                const bp = new Breakpoint(msg.body.verified, msg.body.line, msg.body.column);
                bp.id = msg.body.id;
                this.sendEvent(new BreakpointEvent("changed", bp));
                break;
            }
            case "stopped":
                this.stackFrames = msg.body.stackFrames || [];
                this.sendEvent(new StoppedEvent(msg.body.reason || "pause", THREAD_ID));
                break;
            default:
                this.log(`Event "${msg.event}" has no handler`);
        }
    }

    // Pause execution
    pauseRequest(response, _args, _request) {
        this.sendCommand("pause", {})
            .then(() => this.sendResponse(response))
            .catch((err) => this.sendErrorResponse(response, 1010, `Pause failed: ${err.message}`));
    }

    continueRequest(response, _args) {
        this.sendCommand("continue", {})
            .then(() => this.sendResponse(response))
            .catch((err) => this.sendErrorResponse(response, 1012, `Continue failed: ${err.message}`));
    }

    threadsRequest(response) {
        response.body = {
            threads: [{id: THREAD_ID, name: "MongoDB Shell"}],
        };
        this.sendResponse(response);
    }

    stackTraceRequest(response, _args) {
        this.sendCommand("stackTrace", {})
            .then((result) => {
                // Convert relative paths back to absolute paths so VSCode can open it
                if (result.stackFrames) {
                    const mongoRepo = process.env["MONGO_REPO"];
                    result.stackFrames = result.stackFrames.map((frame) => {
                        if (frame.source?.path && mongoRepo) {
                            // If path is relative, make it absolute
                            if (!frame.source.path.startsWith("/")) {
                                frame.source.path = mongoRepo + "/" + frame.source.path;
                            }
                        }
                        return frame;
                    });
                }
                response.body = result;
                this.sendResponse(response);
            })
            .catch((err) => this.sendErrorResponse(response, 1013, `Stack trace failed: ${err.message}`));
    }

    scopesRequest(response, args) {
        this.sendCommand("scopes", {frameId: args.frameId})
            .then((result) => {
                response.body = result;
                this.sendResponse(response);
            })
            .catch((err) => this.sendErrorResponse(response, 1014, `Scopes failed: ${err.message}`));
    }

    variablesRequest(response, args) {
        this.sendCommand("variables", {variablesReference: args.variablesReference})
            .then((result) => {
                response.body = result;
                this.sendResponse(response);
            })
            .catch((err) => this.sendErrorResponse(response, 1015, `Variables failed: ${err.message}`));
    }

    // Send command to debug server
    sendCommand(command, args) {
        return new Promise((resolve, reject) => {
            if (!this.connected || !this.debugConnection) {
                return reject(new Error("Not connected"));
            }

            const seq = this.messageSeq++;
            this.pendingRequests.set(seq, resolve);

            const msg =
                JSON.stringify({
                    type: "request",
                    seq,
                    command,
                    arguments: args,
                }) + "\n";

            this.debugConnection.write(msg, (err) => {
                if (err) {
                    this.pendingRequests.delete(seq);
                    reject(err);
                }
            });

            // Timeout after 5 seconds
            setTimeout(() => {
                if (this.pendingRequests.has(seq)) {
                    this.pendingRequests.delete(seq);
                    reject(new Error("Command timeout"));
                }
            }, 5_000);
        });
    }
}

module.exports = {MongoShellDebugSession};
