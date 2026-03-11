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
} = require("@vscode/debugadapter");
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

    /**
     * Initialize - tell VSCode what we support
     * @param {DebugProtocol.InitializeResponse} response
     * @overload
     */
    initializeRequest(response) {
        response.body = {
            supportsConfigurationDoneRequest: true,
        };
        this.sendResponse(response);
        this.sendEvent(new InitializedEvent());
    }

    /**
     * Attach - start a debug server, listening for a mongo shell to attach to
     * @param {DebugProtocol.AttachResponse} response
     * @param {DebugProtocol.AttachRequestArguments} args
     * @overload
     */
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
                this.#setupDebugConnection();
            });

            this.debugServer.listen(port, "localhost", () => {
                this.log(`Debug server listening on port ${port}`);
                resolve();
            });

            this.debugServer.on("error", reject);
        });
    }

    // Set up message handling for debug protocol
    #setupDebugConnection() {
        let msgBuffer = "";
        this.debugConnection.on("data", (data) => {
            msgBuffer += data.toString();

            let pos;
            while ((pos = msgBuffer.indexOf("\n")) !== -1) {
                let line = msgBuffer.substring(0, pos);
                msgBuffer = msgBuffer.substring(pos + 1);

                if (!line.trim()) continue;

                try {
                    const msg = JSON.parse(line);
                    this.handleDebugMessage(msg);
                } catch (err) {
                    this.log(`Failed to parse: ${line}`, "stderr");
                    this.log(err.toString(), "stderr");
                }
            }
        });

        this.debugConnection.on("end", () => {
            this.connected = false;
            this.unverifyBreakpoints();
        });

        // Send any queued breakpoints from before connection (attach mode)
        this.sendQueuedBreakpoints();

        this.sendDeferredConfigurationDoneRequest();
    }

    // Send the stored configurationDone request to the attached shells
    sendDeferredConfigurationDoneRequest() {
        const response = {}; // just an acknowledgement
        this.sendCommand("configurationDone", {})
            .then(() => this.sendResponse(response))
            .catch((err) => {
                this.log(`ConfigurationDone failed: ${err.message}`, "stderr");
                this.sendErrorResponse(response, 1011, `Configuration failed: ${err.message}`);
            });
    }

    // Helper to send output to debug console
    log(text, category = "stdout") {
        this.sendEvent(new OutputEvent(text + "\n", category));
    }

    /**
     * Disconnect Request
     * @param {DebugProtocol.DisconnectResponse} response
     * @overload
     */
    disconnectRequest(response) {
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

    /**
     * Breakpoints Request
     * @param {DebugProtocol.SetBreakpointsResponse} response
     * @param {DebugProtocol.SetBreakpointsArguments} args
     * @overload
     */
    setBreakPointsRequest(response, args) {
        const lines = (args.breakpoints || []).map((bp) => ({
            line: bp.line,
            condition: bp.condition,
            logMessage: bp.logMessage,
        }));

        if (this.connected) {
            // Unfortunately the UI still marks this as a red dot in the file.
            // Would be better if we could either not mark the breakpoint, or at least mark it unverified.
            this.sendErrorResponse(
                response,
                1016,
                "New breakpoints are not supported. Breakpoints must be set before the shell launches.",
            );
        }

        // If not connected yet (attach mode), return unverified breakpoints
        // They will be sent to the shell once it connects
        if (!this.connected) {
            const bps = lines.map((bp) => {
                const breakpoint = new Breakpoint(false, bp.line, 0, args.source.path);
                return breakpoint;
            });

            this.breakpoints.set(args.source.path, {lines, unverified: bps});
            response.body = {breakpoints: bps};
            this.sendResponse(response);
            return;
        }
    }

    // Send breakpoints that were set before shell connected
    sendQueuedBreakpoints() {
        for (const [filePath, value] of this.breakpoints.entries()) {
            // Check if these are unverified (queued) breakpoints
            if (value.lines && value.unverified) {
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

    // Reset all breakpoints to unverified state so they can be re-sent when the shell reconnects
    unverifyBreakpoints() {
        for (const [filePath, value] of this.breakpoints.entries()) {
            if (Array.isArray(value)) {
                // Convert verified breakpoints back to unverified queued format
                const lines = value.map((bp) => ({
                    line: bp.line,
                    condition: bp.condition,
                    logMessage: bp.logMessage,
                }));
                const unverified = lines.map((bp) => new Breakpoint(false, bp.line, 0));
                this.breakpoints.set(filePath, {lines, unverified});
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

    /**
     * Pause Request
     * @param {DebugProtocol.PauseResponse} response
     * @overload
     */
    pauseRequest(response) {
        this.sendCommand("pause", {})
            .then(() => this.sendResponse(response))
            .catch((err) => this.sendErrorResponse(response, 1010, `Pause failed: ${err.message}`));
    }

    /**
     * Continue Request
     * @param {DebugProtocol.ContinueResponse} response
     * @overload
     */
    continueRequest(response) {
        this.sendCommand("continue", {})
            .then(() => this.sendResponse(response))
            .catch((err) => this.sendErrorResponse(response, 1012, `Continue failed: ${err.message}`));
    }

    /**
     * Step Over (Next) - stub with auto-continue
     * @param {DebugProtocol.NextResponse} response
     * @param {DebugProtocol.NextArguments} args
     * @overload
     */
    nextRequest(response, args) {
        this.log("Step over is not supported in this debugger. Use continue instead.", "console");
        // Auto-continue to provide seamless UX
        this.continueRequest(response, args);
    }

    /**
     * Step In - stub with auto-continue
     * @param {DebugProtocol.StepInResponse} response
     * @param {DebugProtocol.StepInArguments} args
     * @overload
     */
    stepInRequest(response, args) {
        this.log("Step in is not supported in this debugger. Use continue instead.", "console");
        // Auto-continue to provide seamless UX
        this.continueRequest(response, args);
    }

    /**
     * Step Out - stub with auto-continue
     * @param {DebugProtocol.StepOutResponse} response
     * @param {DebugProtocol.StepOutResponse} args
     * @overload
     */
    stepOutRequest(response, args) {
        this.log("Step out is not supported in this debugger. Use continue instead.", "console");
        // Auto-continue to provide seamless UX
        this.continueRequest(response, args);
    }

    /**
     * Threads Request
     * @param {DebugProtocol.ThreadsResponse} response
     * @overload
     */
    threadsRequest(response) {
        response.body = {
            threads: [{id: THREAD_ID, name: "MongoDB Shell"}],
        };
        this.sendResponse(response);
    }

    /**
     * StackTrace Request
     * @param {DebugProtocol.StackTraceResponse} response
     * @overload
     */
    stackTraceRequest(response) {
        this.sendCommand("stackTrace", {})
            .then((result) => {
                response.body = result;
                this.sendResponse(response);
            })
            .catch((err) => this.sendErrorResponse(response, 1013, `Stack trace failed: ${err.message}`));
    }

    /**
     * Scopes Request
     * @param {DebugProtocol.ScopesResponse} response
     * @param {DebugProtocol.ScopesArguments} args
     * @overload
     */
    scopesRequest(response, args) {
        this.sendCommand("scopes", {frameId: args.frameId})
            .then((result) => {
                response.body = result;
                this.sendResponse(response);
            })
            .catch((err) => this.sendErrorResponse(response, 1014, `Scopes failed: ${err.message}`));
    }

    /**
     * Variables Request
     * @param {DebugProtocol.VariablesResponse} response
     * @param {DebugProtocol.VariablesArguments} args
     * @overload
     */
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
