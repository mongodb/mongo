/**
 * Test TTL docs are not deleted from secondaries directly
 * @tags: [requires_replication]
 */

// Skip db hash check because secondary will have extra document due to the usage of the godinsert
// command.
TestData.skipCheckDBHashes = true;

// setup set
const rt = new ReplSetTest({
    name: "ttl_repl",
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
    ]
});
const nodes = rt.startSet();
rt.initiate();
let master = rt.getPrimary();
rt.awaitSecondaryNodes();
let slave1 = rt.getSecondary();

// shortcuts
let masterdb = master.getDB('d');
let slave1db = slave1.getDB('d');
let mastercol = masterdb['c'];
let slave1col = slave1db['c'];

// create TTL index, wait for TTL monitor to kick in, then check things
mastercol.ensureIndex({x: 1}, {expireAfterSeconds: 10});

rt.awaitReplication();

// increase logging
assert.commandWorked(slave1col.getDB().adminCommand({setParameter: 1, logLevel: 1}));

// insert old doc (10 minutes old) directly on secondary using godinsert
assert.commandWorked(slave1col.runCommand(
    "godinsert", {obj: {_id: new Date(), x: new Date((new Date()).getTime() - 600000)}}));
assert.eq(1, slave1col.count(), "missing inserted doc");

sleep(70 * 1000);  // wait for 70seconds
assert.eq(1, slave1col.count(), "ttl deleted my doc!");

// looking for these errors : "Assertion: 13312:replSet error : logOp() but not primary",
// "replSet error : logOp() but can't accept write to collection <ns>/n" + "Fatal Assertion 17405"
// indicating that the secondary tried to delete the doc, but shouldn't be writing
var errorStrings = ["Assertion: 13312", "Assertion 17405"];
var foundError = false;
var foundLine = "";
var globalLogLines = assert.commandWorked(slave1col.getDB().adminCommand({getLog: "global"})).log;
for (i in globalLogLines) {
    var line = globalLogLines[i];
    errorStrings.forEach(function(errorString) {
        if (line.match(errorString)) {
            foundError = true;
            foundLine = line;  // replace error string with what we found.
        }
    });
}

assert.eq(false, foundError, "found error in this line: " + foundLine);

// finish up
rt.stopSet();
