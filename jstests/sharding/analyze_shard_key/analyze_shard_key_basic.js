/**
 * Tests support for the analyzeShardKey command.
 *
 * @tags: [requires_fcv_70]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    testExistingShardedCollection,
    testExistingUnshardedCollection,
    testNonExistingCollection
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_common_tests.js";

const setParameterOpts = {
    analyzeShardKeyNumRanges: 100
};
const dbNameBase = "testDb";
// The sampling-based initial split policy needs 10 samples per split point so
// 10 * analyzeShardKeyNumRanges is the minimum number of distinct shard key values that the
// collection must have for the command to not fail to generate split points.
const numDocs = 10 * setParameterOpts.analyzeShardKeyNumRanges;

function testNotSupportReadWriteConcern(writeConn, testCases) {
    const dbName = dbNameBase;
    const collName = "testCollReadWriteConcern";
    const ns = dbName + "." + collName;
    const db = writeConn.getDB(dbName);
    const coll = db.getCollection(collName);

    const candidateKey = {candidateKey: 1};
    assert.commandWorked(coll.createIndex(candidateKey));
    assert.commandWorked(coll.insert([{candidateKey: 0}]));
    if (!FixtureHelpers.isStandalone(db)) {
        FixtureHelpers.awaitReplication(db);
    }

    testCases.forEach(testCase => {
        assert.commandFailedWithCode(
            testCase.conn.adminCommand(
                {analyzeShardKey: ns, key: candidateKey, readConcern: {level: "available"}}),
            ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(
            testCase.conn.adminCommand(
                {analyzeShardKey: ns, key: candidateKey, writeConcern: {w: "majority"}}),
            ErrorCodes.InvalidOptions);
    });
}

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 2, setParameter: setParameterOpts}});

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbNameBase, primaryShard: st.shard0.name}));

    const testCases = [];
    // The analyzeShardKey command is supported on mongos and all shardsvr mongods (both primary and
    // secondary).
    testCases.push({conn: st.s, isSupported: true, isMongos: true});
    st.rs0.nodes.forEach(node => {
        testCases.push({conn: node, isSupported: true, isPrimaryShardMongod: true});
    });
    st.rs1.nodes.forEach(node => {
        testCases.push({conn: node, isSupported: true, isPrimaryShardMongod: false});
    });

    st.configRS.nodes.forEach(node => {
        // If config shard mode isn't enabled, don't expect a sharded collection since the config
        // server isn't enabled as a shard and won't have chunks.
        testCases.push({
            conn: node,
            isSupported: true,
            // The config server is shard0 in config shard mode.
            isPrimaryShardMongod: TestData.configShard,
            doNotExpectColl: !TestData.configShard
        });
    });

    testNonExistingCollection(dbNameBase, testCases);
    testExistingUnshardedCollection(dbNameBase, st.s, testCases, numDocs);
    testExistingShardedCollection(dbNameBase, st.s, testCases, numDocs);
    testNotSupportReadWriteConcern(st.s, testCases);

    st.stop();
}

if (jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove tests below
    quit();
}

{
    const rst = new ReplSetTest({
        name: jsTest.name() + "_non_multitenant",
        nodes: 2,
        nodeOptions: {setParameter: setParameterOpts}
    });
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const testCases = [];
    // The analyzeShardKey command is supported on all mongods (both primary and secondary).
    rst.nodes.forEach(node => {
        testCases.push({conn: node, isSupported: true, isReplSetMongod: true});
    });

    testExistingUnshardedCollection(dbNameBase, primary, testCases, numDocs);
    testNonExistingCollection(dbNameBase, testCases);
    testNotSupportReadWriteConcern(primary, testCases);

    rst.stopSet();
}

if (!TestData.auth) {
    const rst = new ReplSetTest({
        name: jsTest.name() + "_multitenant",
        nodes: 1,
        nodeOptions: {
            auth: "",
            setParameter: Object.assign({}, setParameterOpts, {multitenancySupport: true})
        }
    });
    rst.startSet({keyFile: "jstests/libs/key1"});
    rst.initiate();
    const primary = rst.getPrimary();
    const adminDb = primary.getDB("admin");

    // Prepare an authenticated user for testing.
    // Must be authenticated as a user with ActionType::useTenant in order to use security token
    assert.commandWorked(
        adminDb.runCommand({createUser: "admin", pwd: "pwd", roles: ["__system"]}));
    assert(adminDb.auth("admin", "pwd"));

    // The analyzeShardKey command is not supported in multitenancy.
    const testCases = [{conn: adminDb.getMongo(), isSupported: false}];
    testNonExistingCollection("admin", testCases);
    rst.stopSet();
}

{
    const mongod = MongoRunner.runMongod();

    // The analyzeShardKey command is not supported on standalone mongod.
    const testCases = [{conn: mongod, isSupported: false}];
    testExistingUnshardedCollection(dbNameBase, mongod, testCases, numDocs);

    MongoRunner.stopMongod(mongod);
}
