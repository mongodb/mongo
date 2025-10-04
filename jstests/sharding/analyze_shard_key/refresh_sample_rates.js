/**
 * Tests that the _refreshQueryAnalyzerConfiguration command is only supported on the configsvr
 * primary mongod in a sharded cluster and the primary mongod in standalone replica set, and that
 * it returns correct sample rates.
 *
 * @tags: [
 *   requires_fcv_70,
 *   requires_persistence,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {QuerySamplingUtil} from "jstests/sharding/analyze_shard_key/libs/query_sampling_util.js";

// Used to prevent a mongos or mongod from running _refreshQueryAnalyzerConfiguration commands by
// themselves in the background.
const setParameterOpts = {
    "failpoint.disableQueryAnalysisSampler": tojson({mode: "alwaysOn"}),
};

function testBasic(createConnFn, rst, samplerNames) {
    assert.eq(samplerNames.length, 3);
    const primary = rst.getPrimary();
    const conn = createConnFn();

    const dbName = "testDbBasic-" + extractUUIDFromObject(UUID());

    const collName0 = "testColl0";
    const ns0 = dbName + "." + collName0;
    const samplesPerSecond0 = 5;

    const collName1 = "testColl1";
    const ns1 = dbName + "." + collName1;
    const samplesPerSecond1 = 50;

    const db = conn.getDB(dbName);
    assert.commandWorked(db.createCollection(collName0));
    assert.commandWorked(db.createCollection(collName1));
    const collUuid0 = QuerySamplingUtil.getCollectionUuid(db, collName0);
    const collUuid1 = QuerySamplingUtil.getCollectionUuid(db, collName1);

    jsTest.log("Verifying that refreshing returns the correct configurations");
    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns0, mode: "full", samplesPerSecond: samplesPerSecond0}),
    );
    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns1, mode: "full", samplesPerSecond: samplesPerSecond1}),
    );
    const configColl = conn.getCollection("config.queryAnalyzers");
    const startTime0 = configColl.findOne({_id: ns0}).startTime;
    const startTime1 = configColl.findOne({_id: ns1}).startTime;

    // Query distribution after: [1, unknown, unknown]. Verify that refreshing returns
    // samplesPerSecond / numSamplers.
    let res0 = assert.commandWorked(
        primary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: samplerNames[0],
            numQueriesExecutedPerSecond: 1,
        }),
    );
    let expectedRatio0 = 1.0 / 3;
    assert.sameMembers(res0.configurations, [
        {
            ns: ns0,
            collectionUuid: collUuid0,
            samplesPerSecond: expectedRatio0 * samplesPerSecond0,
            startTime: startTime0,
        },
        {
            ns: ns1,
            collectionUuid: collUuid1,
            samplesPerSecond: expectedRatio0 * samplesPerSecond1,
            startTime: startTime1,
        },
    ]);

    // Query distribution after: [1, 0, unknown]. Verify that refreshing returns
    // samplesPerSecond / numSamplers.
    let res1 = assert.commandWorked(
        primary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: samplerNames[1],
            numQueriesExecutedPerSecond: 0, // zero counts as known.
        }),
    );
    let expectedRatio1 = 1.0 / 3;
    assert.sameMembers(res1.configurations, [
        {
            ns: ns0,
            collectionUuid: collUuid0,
            samplesPerSecond: expectedRatio1 * samplesPerSecond0,
            startTime: startTime0,
        },
        {
            ns: ns1,
            collectionUuid: collUuid1,
            samplesPerSecond: expectedRatio1 * samplesPerSecond1,
            startTime: startTime1,
        },
    ]);

    // Query distribution after: [1, 0, 1] (no unknowns). Verify that refreshing returns correct
    // weighted sample rates.
    let res2 = assert.commandWorked(
        primary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: samplerNames[2],
            numQueriesExecutedPerSecond: 1,
        }),
    );
    let expectedRatio2 = 1.0 / 2;
    assert.sameMembers(res2.configurations, [
        {
            ns: ns0,
            collectionUuid: collUuid0,
            samplesPerSecond: expectedRatio2 * samplesPerSecond0,
            startTime: startTime0,
        },
        {
            ns: ns1,
            collectionUuid: collUuid1,
            samplesPerSecond: expectedRatio2 * samplesPerSecond1,
            startTime: startTime1,
        },
    ]);

    // Query distribution after: [4.5, 0, 1] (one is fractional). Verify that refreshing returns
    // correct weighted sample rates.
    res0 = assert.commandWorked(
        primary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: samplerNames[0],
            numQueriesExecutedPerSecond: 4.5,
        }),
    );
    expectedRatio0 = 4.5 / 5.5;
    assert.sameMembers(res0.configurations, [
        {
            ns: ns0,
            collectionUuid: collUuid0,
            samplesPerSecond: expectedRatio0 * samplesPerSecond0,
            startTime: startTime0,
        },
        {
            ns: ns1,
            collectionUuid: collUuid1,
            samplesPerSecond: expectedRatio0 * samplesPerSecond1,
            startTime: startTime1,
        },
    ]);

    // Query distribution after: [4.5, 0, 1] (no change). Verify that refreshing doesn't
    // return any sample rates since the weight for this mongos is 0.
    res1 = assert.commandWorked(
        primary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: samplerNames[1],
            numQueriesExecutedPerSecond: 0,
        }),
    );
    assert.eq(res1.configurations.length, 2);
    assert.sameMembers(res1.configurations, [
        {ns: ns0, collectionUuid: collUuid0, samplesPerSecond: 0, startTime: startTime0},
        {ns: ns1, collectionUuid: collUuid1, samplesPerSecond: 0, startTime: startTime1},
    ]);

    assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns1, mode: "off"}));

    // Query distribution after: [4.5, 0, 1.5]. Verify that refreshing doesn't return a sample
    // rate for the collection with sampling disabled.
    res2 = assert.commandWorked(
        primary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: samplerNames[2],
            numQueriesExecutedPerSecond: 1.5,
        }),
    );
    expectedRatio1 = 1.5 / 6;
    assert.sameMembers(res2.configurations, [
        {
            ns: ns0,
            collectionUuid: collUuid0,
            samplesPerSecond: expectedRatio1 * samplesPerSecond0,
            startTime: startTime0,
        },
    ]);

    assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns0, mode: "off"}));

    // Query distribution after: [4.5, 0, 1.5] (no change).
    res2 = assert.commandWorked(
        primary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: samplerNames[2],
            numQueriesExecutedPerSecond: 0,
        }),
    );
    assert.eq(0, res2.configurations.length);
}

function testFailover(createConnFn, rst, samplerNames) {
    assert.eq(samplerNames.length, 3);
    let primary = rst.getPrimary();
    let conn = createConnFn();

    const dbName = "testDbFailover-" + extractUUIDFromObject(UUID());
    const collName = "testColl";
    const ns = dbName + "." + collName;
    const samplesPerSecond = 5;

    let db = conn.getDB(dbName);
    assert.commandWorked(db.createCollection(collName));
    const collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    jsTest.log("Verify that configurations are persisted and available after failover");
    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: samplesPerSecond}),
    );
    const configColl = conn.getCollection("config.queryAnalyzers");
    const startTime = configColl.findOne({_id: ns}).startTime;

    assert.commandWorked(primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
    primary = rst.getPrimary();
    conn = createConnFn();
    db = conn.getDB(dbName);

    // Query distribution after: [1, unknown, unknown]. Verify that refreshing returns
    // samplesPerSecond / numSamplers.
    let res = assert.commandWorked(
        primary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: samplerNames[0],
            numQueriesExecutedPerSecond: 1,
        }),
    );
    const expectedRatio = 1.0 / 3;
    assert.sameMembers(res.configurations, [
        {
            ns: ns,
            collectionUuid: collUuid,
            samplesPerSecond: expectedRatio * samplesPerSecond,
            startTime,
        },
    ]);

    assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));
}

function testRestart(createConnFn, rst, samplerNames) {
    assert.eq(samplerNames.length, 3);
    let primary = rst.getPrimary();
    let conn = createConnFn();

    const dbName = "testDbRestart-" + extractUUIDFromObject(UUID());
    const collName = "testColl";
    const ns = dbName + "." + collName;
    const samplesPerSecond = 5;

    let db = conn.getDB(dbName);
    assert.commandWorked(db.createCollection(collName));
    const collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    jsTest.log("Verify that configurations are persisted and available after restart");
    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: samplesPerSecond}),
    );
    const configColl = conn.getCollection("config.queryAnalyzers");
    const startTime = configColl.findOne({_id: ns}).startTime;

    rst.stopSet(null /* signal */, true /*forRestart */);
    rst.startSet({restart: true});
    primary = rst.getPrimary();
    conn = createConnFn();
    db = conn.getDB(dbName);

    // Query distribution after: [1, unknown, unknown]. Verify that refreshing returns
    // samplesPerSecond / numSamplers.
    let res = assert.commandWorked(
        primary.adminCommand({
            _refreshQueryAnalyzerConfiguration: 1,
            name: samplerNames[0],
            numQueriesExecutedPerSecond: 1,
        }),
    );
    const expectedRatio = 1.0 / 3;
    assert.sameMembers(res.configurations, [
        {
            ns: ns,
            collectionUuid: collUuid,
            samplesPerSecond: expectedRatio * samplesPerSecond,
            startTime,
        },
    ]);

    assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));
}

function runTest(createConnFn, rst, samplerNames) {
    testBasic(createConnFn, rst, samplerNames);
    // Verify that query distribution info is reset upon stepup.
    assert.commandWorked(rst.getPrimary().adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    testBasic(createConnFn, rst, samplerNames);

    testFailover(createConnFn, rst, samplerNames);
    testRestart(createConnFn, rst, samplerNames);
}

{
    const st = new ShardingTest({
        mongos: {
            s0: {setParameter: setParameterOpts},
            s1: {setParameter: setParameterOpts},
            s2: {setParameter: setParameterOpts},
        },
        shards: 1,
        config: 3,
        rs: {nodes: 1, setParameter: setParameterOpts},
        other: {
            configOptions: {setParameter: setParameterOpts},
        },
        // By default, our test infrastructure sets the election timeout to a very high value (24
        // hours). For this test, we need a shorter election timeout because it relies on nodes
        // running an election when they do not detect an active primary. Therefore, we are setting
        // the electionTimeoutMillis to its default value.
        initiateWithDefaultElectionTimeout: true,
    });
    st.configRS.isConfigSvr = true;
    const samplerNames = [st.s0.host, st.s1.host, st.s2.host];

    jsTest.log("Wait for the config server to be aware that there are 3 mongoses in the cluster");
    assert.soon(() => {
        return st.s.getCollection("config.mongos").find().itcount() == 3;
    });

    jsTest.log(
        "Test that the _refreshQueryAnalyzerConfiguration command is not supported on " +
            "mongos or shardsvr mongod or configsvr secondary mongod",
    );
    const cmdObj = {
        _refreshQueryAnalyzerConfiguration: 1,
        name: samplerNames[0],
        numQueriesExecutedPerSecond: 1,
    };
    assert.commandFailedWithCode(st.s.adminCommand(cmdObj), ErrorCodes.CommandNotFound);
    if (!TestData.configShard) {
        // Shard0 is the config server in config shard mode.
        st.rs0.nodes.forEach((node) => {
            assert.commandFailedWithCode(node.adminCommand(cmdObj), ErrorCodes.IllegalOperation);
        });
    }
    st.configRS.getSecondaries((node) => {
        assert.commandFailedWithCode(node.adminCommand(cmdObj), ErrorCodes.NotWritablePrimary);
    });

    jsTest.log(
        "Test that the _refreshQueryAnalyzerConfiguration command is supported on " + "configsvr primary mongod",
    );
    runTest(() => st.s, st.configRS, samplerNames);

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {
    // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: 3, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});
    const samplerNames = rst.nodes.map((node) => node.host);

    jsTest.log("Test that the _refreshQueryAnalyzerConfiguration command is not supported on " + "secondary mongod");
    const cmdObj = {
        _refreshQueryAnalyzerConfiguration: 1,
        name: samplerNames[0].host,
        numQueriesExecutedPerSecond: 1,
    };
    rst.getSecondaries((node) => {
        assert.commandFailedWithCode(node.adminCommand(cmdObj), ErrorCodes.NotWritablePrimary);
    });

    jsTest.log("Test that the _refreshQueryAnalyzerConfiguration command is supported on " + "primary mongod");
    runTest(() => rst.getPrimary(), rst, samplerNames);

    rst.stopSet();
}
