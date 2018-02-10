// Tests that a client may discover a user's supported SASL mechanisms via isMaster.
(function() {
    "use strict";

    function runTest(conn) {
        var db = conn.getDB("admin");
        var externalDB = conn.getDB("$external");

        // Make users
        assert.commandWorked(db.runCommand({createUser: "user", pwd: "pwd", roles: []}));
        assert.commandWorked(externalDB.runCommand({createUser: "user", roles: []}));

        // Internal users should support SCRAM-SHA-1.
        var isMasterResult =
            assert.commandWorked(db.runCommand({isMaster: 1, saslSupportedMechs: "admin.user"}));
        assert.eq(["SCRAM-SHA-1"], isMasterResult.saslSupportedMechs, tojson(isMasterResult));

        // External users should support PLAIN, but not SCRAM-SHA-1.
        isMasterResult = assert.commandWorked(
            db.runCommand({isMaster: 1, saslSupportedMechs: "$external.user"}));
        assert.eq(["PLAIN"], isMasterResult.saslSupportedMechs, tojson(isMasterResult));
    }

    // Test standalone.
    var m = MongoRunner.runMongod({setParameter: "authenticationMechanisms=SCRAM-SHA-1,PLAIN"});
    runTest(m);
    MongoRunner.stopMongod(m);

    // Test mongos.
    var st = new ShardingTest({
        shards: 0,
        other: {mongosOptions: {setParameter: "authenticationMechanisms=PLAIN,SCRAM-SHA-1"}}
    });
    runTest(st.s0);
    st.stop();
})();
