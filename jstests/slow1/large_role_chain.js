// Tests SERVER-11475 - Make sure server does't crash when many user defined roles are created where
// each role is a member of the next, creating a large chain.

function runTest(conn) {
    var testdb = conn.getDB("rolechain");
    testdb.runCommand({dropAllRolesFromDatabase: 1});
    var chainLen = 2000;

    var buildInfo = conn.getDB("admin").runCommand("buildInfo");
    assert.commandWorked(buildInfo);

    // We reduce the number of roles linked together in the chain to avoid causing this test to take
    // a long time with --dbg=on builds.
    if (buildInfo.debug) {
        chainLen = 200;
    }

    jsTestLog("Generating a chain of " + chainLen + " linked roles");

    var roleNameBase = "chainRole";
    for (var i = 0; i < chainLen; i++) {
        var name = roleNameBase + i;
        if (i == 0) {
            testdb.runCommand({createRole: name, privileges: [], roles: []});
        } else {
            jsTestLog("Creating role " + i);
            var prevRole = roleNameBase + (i - 1);
            testdb.runCommand({createRole: name, privileges: [], roles: [prevRole]});
            var roleInfo = testdb.getRole(name);
        }
    }
}

// run all tests standalone
var conn = MongoRunner.runMongod();
runTest(conn);
MongoRunner.stopMongod(conn);

// run all tests sharded
conn = new ShardingTest({shards: 2, mongos: 1, config: 3});
runTest(conn);
conn.stop();
