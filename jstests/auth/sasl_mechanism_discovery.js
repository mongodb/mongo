// Tests that a client may discover a user's supported SASL mechanisms via hello.
// @tags: [requires_sharding]
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn) {
    const db = conn.getDB("admin");
    const externalDB = conn.getDB("$external");

    function getStatCounter() {
        const res = assert.commandWorked(db.runCommand({serverStatus: 1}));
        return res.security.authentication.saslSupportedMechsReceived;
    }

    function checkMechs(userid, mechs) {
        const lastCounterValue = getStatCounter();

        const res = assert.commandWorked(db.runCommand({hello: 1, saslSupportedMechs: userid}));

        const counterValue = getStatCounter();
        assert.eq(counterValue, lastCounterValue + 1);

        assert.eq(mechs.sort(), res.saslSupportedMechs.sort(), tojson(res));
    }

    assert.commandWorked(
        db.runCommand({
            createUser: "userAdmin",
            pwd: "userAdmin",
            roles: ["userAdminAnyDatabase", "clusterMonitor"],
        }),
    );
    db.auth("userAdmin", "userAdmin");

    // Check that unknown users do not interrupt hello
    let res = assert.commandWorked(db.runCommand({hello: 1, saslSupportedMechs: "test.bogus"}));
    assert.eq(undefined, res.saslSupportedMechs);

    // Check that invalid usernames produce the correct error code
    assert.commandFailedWithCode(db.runCommand({hello: 1, saslSupportedMechs: "bogus"}), ErrorCodes.BadValue);

    assert.commandWorked(db.runCommand({createUser: "user", pwd: "pwd", roles: []}));
    assert.commandWorked(externalDB.runCommand({createUser: "user", roles: []}));

    // Internal users should support scram methods.
    checkMechs("admin.user", ["SCRAM-SHA-256", "SCRAM-SHA-1"]);

    // External users on enterprise should support PLAIN, but not scram methods.
    if (assert.commandWorked(db.runCommand({buildInfo: 1})).modules.includes("enterprise")) {
        checkMechs("$external.user", ["PLAIN"]);
    } else {
        checkMechs("$external.user", []);
    }

    // Users with explicit mechs should only support those mechanisms
    assert.commandWorked(db.runCommand({createUser: "256Only", pwd: "pwd", roles: [], mechanisms: ["SCRAM-SHA-256"]}));
    checkMechs("admin.256Only", ["SCRAM-SHA-256"]);
    assert.commandWorked(db.runCommand({createUser: "1Only", pwd: "pwd", roles: [], mechanisms: ["SCRAM-SHA-1"]}));
    checkMechs("admin.1Only", ["SCRAM-SHA-1"]);

    // Users with normalized and unnormalized names do not conflict
    assert.commandWorked(db.runCommand({createUser: "IX", pwd: "pwd", roles: []}));
    checkMechs("admin.IX", ["SCRAM-SHA-1", "SCRAM-SHA-256"]);
    assert.commandWorked(db.runCommand({createUser: "\u2168", pwd: "pwd", roles: []}));
    checkMechs("admin.\u2168", ["SCRAM-SHA-1", "SCRAM-SHA-256"]);

    // __system's mechanisms can be queried on local and admin if the server is in test mode
    checkMechs("local.__system", ["SCRAM-SHA-256"]);
    checkMechs("admin.__system", ["SCRAM-SHA-256"]);
}

// Test standalone.
let m = MongoRunner.runMongod({
    keyFile: "jstests/libs/key1",
    setParameter: {authenticationMechanisms: "SCRAM-SHA-1,SCRAM-SHA-256,PLAIN"},
});
runTest(m);
MongoRunner.stopMongod(m);

// Test mongos.
if (TestData.configShard) {
    // Config shard requires at least one shard.
    quit();
}
let st = new ShardingTest({
    keyFile: "jstests/libs/key1",
    shards: 0,
    other: {
        mongosOptions: {setParameter: {authenticationMechanisms: "PLAIN,SCRAM-SHA-256,SCRAM-SHA-1"}},
    },
});
runTest(st.s0);
st.stop();
