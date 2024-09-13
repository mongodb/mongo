/**
 * Tests that serverStatus metrics correctly print commands.hello if the user sends hello
 * and commands.isMaster if the user sends isMaster or ismaster
 */
const mongod = MongoRunner.runMongod();
const dbName = "server_status_metrics_hello_command";
const db = mongod.getDB(dbName);
let serverStatusMetrics = db.serverStatus().metrics.commands;

function getCommandCount(cmdName) {
    return serverStatusMetrics.hasOwnProperty(cmdName) ? serverStatusMetrics[cmdName].total : 0;
}

let currentIsMasterTotal = getCommandCount("isMaster");
let currentHelloTotal = getCommandCount("hello");

// Running hello command.
jsTestLog("Running hello command");
assert.commandWorked(db.runCommand({hello: 1}));
serverStatusMetrics = db.serverStatus().metrics.commands;
assert.eq(getCommandCount("hello"), currentHelloTotal + 1, "commands.hello should increment");
++currentHelloTotal;
assert.eq(
    getCommandCount("isMaster"), currentIsMasterTotal, "commands.isMaster should not increment");

// Running isMaster command.
jsTestLog("Running isMaster command");
assert.commandWorked(db.runCommand({isMaster: 1}));
serverStatusMetrics = db.serverStatus().metrics.commands;
assert.eq(getCommandCount("hello"), currentHelloTotal, "commands.hello should not increment");
assert.eq(
    getCommandCount("isMaster"), currentIsMasterTotal + 1, "commands.isMaster should increment");
++currentIsMasterTotal;

// Running ismaster command.
jsTestLog("Running ismaster command");
assert.commandWorked(db.runCommand({ismaster: 1}));
serverStatusMetrics = db.serverStatus().metrics.commands;
assert.eq(getCommandCount("hello"), currentHelloTotal, "commands.hello should not increment");
assert.eq(
    getCommandCount("isMaster"), currentIsMasterTotal + 1, "commands.isMaster should increment");
++currentIsMasterTotal;

MongoRunner.stopMongod(mongod);