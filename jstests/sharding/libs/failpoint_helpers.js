// Helper functions for failpoint tests.
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

/**
 * Hang specific commands before starting their execution on a given node
 */
export const hangCommandBeforeExecution = function(
    node, commandToFail, hangInternalCommands = true) {
    let failPointData = {
        failCommands: [commandToFail],
        blockConnection: true,
        failInternalCommands: hangInternalCommands
    };
    return configureFailPoint(node, "failCommand", failPointData);
};

export const failCommandWithError = function(rst, {commandToFail, errorCode, closeConnection}) {
    rst.nodes.forEach(function(node) {
        assert.commandWorked(node.getDB("admin").runCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                closeConnection: closeConnection,
                errorCode: errorCode,
                failCommands: [commandToFail],
                failInternalCommands: true  // mongod sees mongos as an internal client
            }
        }));
    });
};

export const failCommandWithWriteConcernError = function(rst, commandToFail) {
    if (typeof commandToFail === "string") {
        commandToFail = [commandToFail];
    }
    failCommandsWithWriteConcernError(rst, commandToFail);
};

export const failCommandsWithWriteConcernError = function(rst, commandsToFail) {
    rst.nodes.forEach(function(node) {
        assert.commandWorked(node.getDB("admin").runCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                writeConcernError: {code: NumberInt(12345), errmsg: "dummy"},
                failCommands: commandsToFail,
                failInternalCommands: true  // mongod sees mongos as an internal client
            }
        }));
    });
};

export const turnOffFailCommand = function(rst) {
    rst.nodes.forEach(function(node) {
        assert.commandWorked(
            node.getDB("admin").runCommand({configureFailPoint: "failCommand", mode: "off"}));
    });
};
