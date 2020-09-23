/**
 * Tests that serverStatus metrics correctly print commands.hello if the user sends hello
 * and commands.isMaster if the user sends isMaster or ismaster
 */
(function() {
"use strict";
const mongod = MongoRunner.runMongod();
const dbName = "server_status_metrics_hello_command";
const db = mongod.getDB(dbName);
let serverStatusMetrics = db.serverStatus().metrics.commands;
const initialIsMasterTotal = serverStatusMetrics.isMaster.total;
const initialHelloTotal = 0;

// Running hello command.
jsTestLog("Running hello command");
assert.commandWorked(db.runCommand({hello: 1}));
serverStatusMetrics = db.serverStatus().metrics.commands;
assert.eq(
    serverStatusMetrics.hello.total, initialHelloTotal + 1, "commands.hello should increment");
assert.eq(serverStatusMetrics.isMaster.total,
          initialIsMasterTotal,
          "commands.isMaster should not increment");

// Running isMaster command.
jsTestLog("Running isMaster command");
assert.commandWorked(db.runCommand({isMaster: 1}));
serverStatusMetrics = db.serverStatus().metrics.commands;
assert.eq(
    serverStatusMetrics.hello.total, initialHelloTotal + 1, "commands.hello should not increment");
assert.eq(serverStatusMetrics.isMaster.total,
          initialIsMasterTotal + 1,
          "commands.isMaster should increment");

// Running ismaster command.
jsTestLog("Running ismaster command");
assert.commandWorked(db.runCommand({ismaster: 1}));
serverStatusMetrics = db.serverStatus().metrics.commands;
assert.eq(
    serverStatusMetrics.hello.total, initialHelloTotal + 1, "commands.hello should not increment");
assert.eq(serverStatusMetrics.isMaster.total,
          initialIsMasterTotal + 2,
          "commands.isMaster should increment");
MongoRunner.stopMongod(mongod);
})();
