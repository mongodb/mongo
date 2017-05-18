/**
 * A Promise-like interface for running a sequence of commands.
 *
 * If a network error occurs while running a command, then a reconnect is automatically
 * attempted and the sequence will resume by sending the same command again. Any other errors
 * that occur while running a command will cause the entire sequence of commands to abort.
 *
 * @param {Mongo} conn - a connection to the server
 */
function CommandSequenceWithRetries(conn) {
    "use strict";

    if (!(this instanceof CommandSequenceWithRetries)) {
        return new CommandSequenceWithRetries(conn);
    }

    const steps = [];

    function attemptReconnect(conn) {
        try {
            conn.adminCommand({ping: 1});
        } catch (e) {
            return false;
        }
        return true;
    }

    this.then = function then(phase, action) {
        steps.push({phase: phase, action: action});
        return this;
    };

    this.execute = function execute() {
        let i = 0;
        let stepHadNetworkErrorAlready = false;

        while (i < steps.length) {
            try {
                // Treat no explicit return statement inside the action function as returning
                // {shouldStop: false} for syntactic convenience.
                const result = steps[i].action(conn);
                if (result !== undefined && result.shouldStop) {
                    return {ok: 0, msg: "giving up after " + steps[i].phase + ": " + result.reason};
                }
            } catch (e) {
                if (!isNetworkError(e)) {
                    throw e;
                }

                // We retry running the action function a second time after a network error
                // because it is possible that the node is in the process of stepping down. We
                // won't be able to reconnect to the node until it has finished closing all of
                // its open connections.
                if (stepHadNetworkErrorAlready) {
                    return {
                        ok: 0,
                        msg: "giving up after " + steps[i].phase +
                            " because we encountered multiple network errors"
                    };
                }

                if (!attemptReconnect(conn)) {
                    return {
                        ok: 0,
                        msg: "giving up after " + steps[i].phase +
                            " because attempting to reconnect failed"
                    };
                }

                stepHadNetworkErrorAlready = true;
                continue;
            }

            ++i;
            stepHadNetworkErrorAlready = false;
        }

        return {ok: 1};
    };
}
