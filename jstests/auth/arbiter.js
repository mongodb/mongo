// Certain commands should be run-able from arbiters under localhost, but not from
// any other nodes in the replset.

var name = "arbiter_localhost_test";
var key = "jstests/libs/key1";
var replTest = new ReplSetTest({name: name, nodes: 3, keyFile: key});
var nodes = replTest.nodeList();

replTest.startSet();
replTest.initiate({
    _id: name,
    members: [
        {"_id": 0, "host": nodes[0], priority: 3},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], arbiterOnly: true}
    ],
});

var primaryAdmin = replTest.nodes[0].getDB("admin");
var arbiterAdmin = replTest.nodes[2].getDB("admin");

var cmd0 = {getCmdLineOpts: 1};
var cmd1 = {getParameter: 1, logLevel: 1};
var cmd2 = {serverStatus: 1};

assert.commandFailedWithCode(primaryAdmin.runCommand(cmd0), 13);
assert.commandFailedWithCode(primaryAdmin.runCommand(cmd1), 13);
assert.commandFailedWithCode(primaryAdmin.runCommand(cmd2), 13);

assert.commandWorked(arbiterAdmin.runCommand(cmd0));
assert.commandWorked(arbiterAdmin.runCommand(cmd1));
assert.commandWorked(arbiterAdmin.runCommand(cmd2));
