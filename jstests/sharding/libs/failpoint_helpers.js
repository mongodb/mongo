// Helper functions for failpoint tests.

const failCommandWithError = function(rst, {commandToFail, errorCode, closeConnection}) {
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

const failCommandWithWriteConcernError = function(rst, commandToFail) {
    if (typeof commandToFail === "string") {
        commandToFail = [commandToFail];
    }
    failCommandsWithWriteConcernError(rst, commandToFail);
};

const failCommandsWithWriteConcernError = function(rst, commandsToFail) {
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

const turnOffFailCommand = function(rst) {
    rst.nodes.forEach(function(node) {
        assert.commandWorked(
            node.getDB("admin").runCommand({configureFailPoint: "failCommand", mode: "off"}));
    });
};
