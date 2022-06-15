(function() {
'use strict';

const runTest = function(conn, failPointConn) {
    jsTestLog("Setting up users");
    const db = conn.getDB("admin");
    assert.commandWorked(
        db.runCommand({createUser: "admin", pwd: "pwd", roles: jsTest.adminUserRoles}));
    assert.eq(db.auth("admin", "pwd"), 1);
    assert.commandWorked(db.runCommand({createUser: "testuser", pwd: "pwd", roles: []}));
    db.grantRolesToUser("testuser", [{role: "readWrite", db: "test"}]);

    assert.commandWorked(db.getSiblingDB("test").test.insert({}));

    jsTestLog("blocking finds and starting parallel shell to create op");
    assert.commandWorked(failPointConn.getDB("admin").runCommand(
        {configureFailPoint: "waitInFindBeforeMakingBatch", mode: "alwaysOn"}));
    let finderWait = startParallelShell(function() {
        assert.eq(db.getSiblingDB("admin").auth("testuser", "pwd"), 1);
        let testDB = db.getSiblingDB("test");
        assert.eq(testDB.test.find({}).comment("curop_auth_info.js query").itcount(), 1);
    }, conn.port);

    let myOp;
    assert.soon(function() {
        const curOpResults = assert.commandWorked(db.runCommand({currentOp: 1}));
        print(tojson(curOpResults));
        const myOps = curOpResults["inprog"].filter((op) => {
            return (op["command"]["comment"] == "curop_auth_info.js query");
        });

        if (myOps.length == 0) {
            return false;
        }
        myOp = myOps[0];
        return true;
    });

    jsTestLog("found op");
    assert.commandWorked(failPointConn.getDB("admin").runCommand(
        {configureFailPoint: "waitInFindBeforeMakingBatch", mode: "off"}));
    finderWait();

    const authedUsers = myOp["effectiveUsers"];
    const impersonators = myOp["runBy"];
    print(tojson(authedUsers), tojson(impersonators));
    if (impersonators) {
        assert.eq(authedUsers.length, 1);
        assert.docEq(authedUsers[0], {user: "testuser", db: "admin"});
        assert(impersonators);
        assert.eq(impersonators.length, 1);
        assert.docEq(impersonators[0], {user: "__system", db: "local"});
    } else {
        assert(authedUsers);
        assert.eq(authedUsers.length, 1);
        assert.docEq(authedUsers[0], {user: "testuser", db: "admin"});
    }
};

const m = MongoRunner.runMongod();
runTest(m, m);
MongoRunner.stopMongod(m);

if (jsTestOptions().storageEngine != "inMemory") {
    const st = new ShardingTest({shards: 1, mongos: 1, config: 1, keyFile: 'jstests/libs/key1'});
    runTest(st.s0, st.shard0);
    st.stop();
}
})();
