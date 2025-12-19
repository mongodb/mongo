/**
 * Writer class for executing commands and notifying completion.
 * Executes a sequence of commands and signals test completion via the Connector.
 */
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";

class Writer {
    /**
     * Execute all commands in the configuration and notify completion.
     * @param {Mongo} conn - MongoDB connection.
     * @param {Object} config - Configuration object containing:
     *   - commands: Array of command objects to execute
     *   - instanceName: Name of the test instance (also used as collection name)
     */
    static run(conn, config) {
        for (const cmd of config.commands) {
            cmd.execute(conn);
        }

        Connector.notifyDone(conn, config.instanceName);
    }
}

export {Writer};
