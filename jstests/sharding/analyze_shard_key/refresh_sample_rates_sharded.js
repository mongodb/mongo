/**
 * Tests that the _refreshQueryAnalyzerConfiguration command is only supported on the config
 * server's primary and that it returns correct sample rates.
 *
 * @tags: [requires_fcv_62, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

// Prevent all mongoses from running _refreshQueryAnalyzerConfiguration commands by themselves in
// the background.
const setParameterOpts = {
    setParameter: {"failpoint.disableQueryAnalysisSampler": tojson({mode: "alwaysOn"})}
};
const st = new ShardingTest(
    {mongos: {s0: setParameterOpts, s1: setParameterOpts, s2: setParameterOpts}, shards: 1});

const dbName = "testDb";
const db = st.s0.getDB(dbName);

const collName0 = "testColl0";
const ns0 = dbName + "." + collName0;
const sampleRate0 = 5;

const collName1 = "testColl1";
const ns1 = dbName + "." + collName1;
const sampleRate1 = 50;

assert.commandWorked(db.createCollection(collName0));
assert.commandWorked(db.createCollection(collName1));

function getCollectionUuid(collName) {
    const listCollectionRes =
        assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
    return listCollectionRes.cursor.firstBatch[0].info.uuid;
}
const collUuid0 = getCollectionUuid(collName0);
const collUuid1 = getCollectionUuid(collName1);

{
    jsTest.log("Test that the _refreshQueryAnalyzerConfiguration command is not supported on " +
               "mongos or shardsvr mongod or configsvr secondary mongod");

    function runTest(conn, expectedErrCode) {
        assert.commandWorked(st.s0.adminCommand(
            {configureQueryAnalyzer: ns0, mode: "full", sampleRate: sampleRate0}));
        assert.commandWorked(st.s0.adminCommand(
            {configureQueryAnalyzer: ns1, mode: "full", sampleRate: sampleRate1}));

        assert.commandFailedWithCode(conn.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: st.s0.host,
            numQueriesExecutedPerSecond: 1
        }),
                                     expectedErrCode);

        assert.commandWorked(st.s0.adminCommand({configureQueryAnalyzer: ns0, mode: "off"}));
        assert.commandWorked(st.s0.adminCommand({configureQueryAnalyzer: ns1, mode: "off"}));
    }

    runTest(st.s0, ErrorCodes.CommandNotFound);
    st.rs0.nodes.forEach(node => {
        runTest(node, ErrorCodes.IllegalOperation);
    });
    st.configRS.getSecondaries(node => {
        runTest(node, ErrorCodes.NotWritablePrimary);
    });
}

{
    jsTest.log("Test that the _refreshQueryAnalyzerConfiguration command is supported on " +
               "configsvr primary mongod");

    function runTestBasic(configRSPrimary) {
        jsTest.log("Verifying that refreshing returns the correct configurations: " +
                   configRSPrimary);
        assert.commandWorked(st.s0.adminCommand(
            {configureQueryAnalyzer: ns0, mode: "full", sampleRate: sampleRate0}));
        assert.commandWorked(st.s0.adminCommand(
            {configureQueryAnalyzer: ns1, mode: "full", sampleRate: sampleRate1}));

        // Query distribution after: [1, unknown, unknown]. Verify that refreshing returns
        // sampleRate / numMongoses.
        let res0 = assert.commandWorked(configRSPrimary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: st.s0.host,
            numQueriesExecutedPerSecond: 1
        }));
        let expectedRatio0 = 1.0 / 3;
        assert.sameMembers(res0.configurations, [
            {ns: ns0, collectionUuid: collUuid0, sampleRate: expectedRatio0 * sampleRate0},
            {ns: ns1, collectionUuid: collUuid1, sampleRate: expectedRatio0 * sampleRate1},
        ]);

        // Query distribution after: [1, 0, unknown]. Verify that refreshing returns
        // sampleRate / numMongoses.
        let res1 = assert.commandWorked(configRSPrimary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: st.s1.host,
            numQueriesExecutedPerSecond: 0  // zero counts as known.
        }));
        let expectedRatio1 = 1.0 / 3;
        assert.sameMembers(res1.configurations, [
            {ns: ns0, collectionUuid: collUuid0, sampleRate: expectedRatio1 * sampleRate0},
            {ns: ns1, collectionUuid: collUuid1, sampleRate: expectedRatio1 * sampleRate1},
        ]);

        // Query distribution after: [1, 0, 1] (no unknowns). Verify that refreshing returns correct
        // weighted sample rates.
        let res2 = assert.commandWorked(configRSPrimary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: st.s2.host,
            numQueriesExecutedPerSecond: 1
        }));
        let expectedRatio2 = 1.0 / 2;
        assert.sameMembers(res2.configurations, [
            {ns: ns0, collectionUuid: collUuid0, sampleRate: expectedRatio2 * sampleRate0},
            {ns: ns1, collectionUuid: collUuid1, sampleRate: expectedRatio2 * sampleRate1},
        ]);

        // Query distribution after: [4.5, 0, 1] (one is fractional). Verify that refreshing returns
        // correct weighted sample rates.
        res0 = assert.commandWorked(configRSPrimary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: st.s0.host,
            numQueriesExecutedPerSecond: 4.5
        }));
        expectedRatio0 = 4.5 / 5.5;
        assert.sameMembers(res0.configurations, [
            {ns: ns0, collectionUuid: collUuid0, sampleRate: expectedRatio0 * sampleRate0},
            {ns: ns1, collectionUuid: collUuid1, sampleRate: expectedRatio0 * sampleRate1},
        ]);

        // Query distribution after: [4.5, 0, 1] (no change). Verify that refreshing returns correct
        // weighted sample rates (i.e. zero).
        res1 = assert.commandWorked(configRSPrimary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: st.s1.host,
            numQueriesExecutedPerSecond: 0
        }));
        expectedRatio1 = 0;
        assert.sameMembers(res1.configurations, [
            {ns: ns0, collectionUuid: collUuid0, sampleRate: expectedRatio1 * sampleRate0},
            {ns: ns1, collectionUuid: collUuid1, sampleRate: expectedRatio1 * sampleRate1},
        ]);

        assert.commandWorked(st.s0.adminCommand({configureQueryAnalyzer: ns1, mode: "off"}));

        // Query distribution after: [4.5, 0, 1.5]. Verify that refreshing doesn't return a sample
        // rate for the collection with sampling disabled.
        res2 = assert.commandWorked(configRSPrimary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: st.s2.host,
            numQueriesExecutedPerSecond: 1.5
        }));
        expectedRatio1 = 1.5 / 6;
        assert.sameMembers(res2.configurations, [
            {ns: ns0, collectionUuid: collUuid0, sampleRate: expectedRatio1 * sampleRate0},
        ]);

        assert.commandWorked(st.s0.adminCommand({configureQueryAnalyzer: ns0, mode: "off"}));

        // Query distribution after: [4.5, 0, 1.5] (no change).
        res2 = assert.commandWorked(configRSPrimary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: st.s2.host,
            numQueriesExecutedPerSecond: 0
        }));
        assert.eq(0, res2.configurations.length);
    }

    function runTestFailover(configRS) {
        jsTest.log("Verify that configurations are persisted and available after failover");
        assert.commandWorked(st.s0.adminCommand(
            {configureQueryAnalyzer: ns0, mode: "full", sampleRate: sampleRate0}));
        assert.commandWorked(st.s0.adminCommand({configureQueryAnalyzer: ns1, mode: "off"}));

        let configRSPrimary = configRS.getPrimary();
        assert.commandWorked(
            configRSPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(configRSPrimary.adminCommand({replSetFreeze: 0}));
        configRSPrimary = configRS.getPrimary();

        // Query distribution after: [1, unknown, unknown]. Verify that refreshing returns
        // sampleRate / numMongoses.
        let res = assert.commandWorked(configRSPrimary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: st.s0.host,
            numQueriesExecutedPerSecond: 1
        }));
        const expectedRatio = 1.0 / 3;
        assert.sameMembers(
            res.configurations,
            [{ns: ns0, collectionUuid: collUuid0, sampleRate: expectedRatio * sampleRate0}]);
    }

    function runTestRestart(configRS) {
        jsTest.log("Verify that configurations are persisted and available after restart");
        assert.commandWorked(st.s0.adminCommand(
            {configureQueryAnalyzer: ns0, mode: "full", sampleRate: sampleRate0}));
        assert.commandWorked(st.s0.adminCommand({configureQueryAnalyzer: ns1, mode: "off"}));

        configRS.stopSet(null /* signal */, true /*forRestart */);
        configRS.startSet({restart: true});
        const configRSPrimary = configRS.getPrimary();

        // Query distribution after: [1, unknown, unknown]. Verify that refreshing returns
        // sampleRate / numMongoses.
        let res = assert.commandWorked(configRSPrimary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: st.s0.host,
            numQueriesExecutedPerSecond: 1
        }));
        const expectedRatio = 1.0 / 3;
        assert.sameMembers(
            res.configurations,
            [{ns: ns0, collectionUuid: collUuid0, sampleRate: expectedRatio * sampleRate0}]);
    }

    let configRSPrimary = st.configRS.getPrimary();
    runTestBasic(configRSPrimary);
    // Verify that query distribution info is reset upon stepup.
    assert.commandWorked(
        configRSPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(configRSPrimary.adminCommand({replSetFreeze: 0}));
    configRSPrimary = st.configRS.getPrimary();
    runTestBasic(configRSPrimary);

    runTestFailover(st.configRS);
    runTestRestart(st.configRS);
}

st.stop();
})();
