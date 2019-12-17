(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn);
const db = conn.getDB("test_failcommand_noparallel");

// Test times when closing connection.
// Use distinct because it is rarely used by internal operations, making it less likely unrelated
// activity triggers the failpoint.
assert.commandWorked(db.adminCommand({
    configureFailPoint: "failCommand",
    mode: {times: 2},
    data: {
        closeConnection: true,
        failCommands: ["distinct"],
    }
}));
assert.throws(() => db.runCommand({distinct: "c", key: "_id"}));
assert.throws(() => db.runCommand({distinct: "c", key: "_id"}));
assert.commandWorked(db.runCommand({distinct: "c", key: "_id"}));
assert.commandWorked(db.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

MongoRunner.stopMongod(conn);
}());
