/**
 * Tests support for the configureQueryAnalyzer command.
 *
 * @tags: [requires_fcv_62, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

function testNonExistingCollection(conn, ns) {
    jsTest.log(`Running configureQueryAnalyzer command against an non-existing collection ${
        ns} on ${conn}`);
    assert.commandFailedWithCode(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1}),
        ErrorCodes.NamespaceNotFound);
}

function testExistingCollection(conn, ns) {
    jsTest.log(
        `Running configureQueryAnalyzer command against an existing collection ${ns} on ${conn}`);

    // Cannot set 'sampleRate' to 0.
    assert.commandFailedWithCode(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 0}),
        ErrorCodes.InvalidOptions);

    // Can set 'sampleRate' to > 0.
    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 0.1}));
    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1}));
    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1000}));

    // Cannot specify 'sampleRate' when 'mode' is "off".
    assert.commandFailedWithCode(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "off", sampleRate: 1}),
        ErrorCodes.InvalidOptions);
    assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

    // Cannot specify read/write concern.
    assert.commandFailedWithCode(conn.adminCommand({
        configureQueryAnalyzer: ns,
        mode: "full",
        sampleRate: 1,
        readConcern: {level: "available"}
    }),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(conn.adminCommand({
        configureQueryAnalyzer: ns,
        mode: "full",
        sampleRate: 1,
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.InvalidOptions);
}

{
    const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
    const shard0Primary = st.rs0.getPrimary();
    const shard0Secondaries = st.rs0.getSecondaries();
    const configPrimary = st.configRS.getPrimary();
    const configSecondaries = st.configRS.getSecondaries();

    const dbName = "testDb";
    const nonExistingNs = dbName + ".nonExistingColl";

    const shardedNs = dbName + ".shardedColl";
    const shardKey = {key: 1};
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);
    assert.commandWorked(st.s.adminCommand({shardCollection: shardedNs, key: shardKey}));

    const unshardedCollName = "unshardedColl";
    const unshardedNs = dbName + "." + unshardedCollName;
    assert.commandWorked(st.s.getDB(dbName).createCollection(unshardedCollName));

    // Verify that the command is supported on mongos and configsvr primary mongod.
    function testSupported(conn) {
        testNonExistingCollection(conn, nonExistingNs);
        testExistingCollection(conn, unshardedNs);
        testExistingCollection(conn, shardedNs);
    }

    testSupported(st.s);
    testSupported(configPrimary);

    // Verify that the command is not supported on configsvr secondary mongods or any shardvr
    // mongods.
    function testNotSupported(conn, errorCode) {
        assert.commandFailedWithCode(
            conn.adminCommand({configureQueryAnalyzer: unshardedNs, mode: "full", sampleRate: 1}),
            errorCode);
        assert.commandFailedWithCode(
            conn.adminCommand({configureQueryAnalyzer: shardedNs, mode: "full", sampleRate: 1}),
            errorCode);
    }

    configSecondaries.forEach(node => {
        testNotSupported(node, ErrorCodes.NotWritablePrimary);
    });
    testNotSupported(shard0Primary, ErrorCodes.IllegalOperation);
    shard0Secondaries.forEach(node => {
        testNotSupported(node, ErrorCodes.NotWritablePrimary);
    });

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const secondaries = rst.getSecondaries();

    const dbName = "testDb";
    const nonExistingNs = dbName + ".nonExistingColl";

    const unshardedCollName = "unshardedColl";
    const unshardedNs = dbName + "." + unshardedCollName;
    assert.commandWorked(primary.getDB(dbName).createCollection(unshardedCollName));

    // Verify that the command is supported on primary mongod.
    function testSupported(conn) {
        testNonExistingCollection(conn, nonExistingNs);
        testExistingCollection(conn, unshardedNs);
    }

    testSupported(primary, unshardedNs);

    // Verify that the command is not supported on secondary mongods.
    function testNotSupported(conn, errorCode) {
        assert.commandFailedWithCode(
            conn.adminCommand({configureQueryAnalyzer: unshardedNs, mode: "full", sampleRate: 1}),
            errorCode);
    }

    secondaries.forEach(node => {
        testNotSupported(node, ErrorCodes.NotWritablePrimary);
    });

    rst.stopSet();
}
})();
