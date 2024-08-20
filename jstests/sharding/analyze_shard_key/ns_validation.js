/**
 * Tests that the configureQueryAnalyzer command, the analyzeShardKey command, the
 * $listSampledQueries aggregate stage and $_analyzeShardKeyReadWriteDistribution
 * stage validate that the namespace is correctly formatted.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

function makeTestConfigureAnalyzerCmdObj(ns) {
    return {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 10};
}

function makeTestAnalyzeShardKeyCmdObj(ns) {
    return {analyzeShardKey: ns, key: {x: 1}};
}

function makeTestListSampledQueriesAggregateCmdObj(ns) {
    return {aggregate: 1, pipeline: [{$listSampledQueries: {namespace: ns}}], cursor: {}};
}

function makeTestAnalyzeShardKeyAggregateCmdObj(ns) {
    const splitNs = ns.split(".");
    return {
        aggregate: splitNs.length == 1 ? "" : splitNs[1],
        pipeline: [{
            $_analyzeShardKeyReadWriteDistribution: {
                key: {x: 1},
                splitPointsFilter: {"_id.analyzeShardKeyId": UUID()},
                splitPointsAfterClusterTime: new Timestamp(100, 1),
                splitPointsShardId: "shard0"
            }
        }],
        cursor: {}
    };
}

function runTestForCmd(db, makeCmdObjFunc, testDbName, testCollName, requiresCollectionToExist) {
    const testNs = testDbName + "." + testCollName;

    const res1 = db.runCommand(makeCmdObjFunc(testNs));
    jsTest.log("*** Response for command: " + tojsononeline({db, ns: testNs, response: res1}));
    if (requiresCollectionToExist) {
        assert.commandFailedWithCode(res1, ErrorCodes.NamespaceNotFound);
    } else {
        assert.commandWorked(res1);
    }

    const res2 = db.runCommand(makeCmdObjFunc(testDbName));
    jsTest.log("*** Response for command: " + tojsononeline({db, ns: testDbName, response: res2}));
    assert.commandFailedWithCode(res2, ErrorCodes.InvalidNamespace);

    const res3 = db.runCommand(makeCmdObjFunc(testCollName));
    jsTest.log("*** Response for command: " +
               tojsononeline({db, ns: testCollName, response: res3}));
    assert.commandFailedWithCode(res3, ErrorCodes.InvalidNamespace);
}

function runTests(conn, rst) {
    const testDbName = "testDb-" + extractUUIDFromObject(UUID());
    const testCollName0 = "testColl0";
    const testCollName1 = "testColl1";

    const adminDb = conn.getDB("admin");
    const testDb = conn.getDB(testDbName);

    const primary = rst.getPrimary();
    const primaryTestDb = primary.getDB(testDbName);
    const isReplicaSetEndpointActive = rst.isReplicaSetEndpointActive();

    runTestForCmd(adminDb,
                  makeTestConfigureAnalyzerCmdObj,
                  testDbName,
                  testCollName0,
                  true /* requiresCollectionToExist */);
    runTestForCmd(adminDb,
                  makeTestAnalyzeShardKeyCmdObj,
                  testDbName,
                  testCollName0,
                  true /* requiresCollectionToExist */);
    runTestForCmd(adminDb,
                  makeTestListSampledQueriesAggregateCmdObj,
                  testDbName,
                  testCollName0,
                  false /* requiresCollectionToExist */);
    runTestForCmd(primaryTestDb,
                  makeTestAnalyzeShardKeyAggregateCmdObj,
                  testDbName,
                  testCollName0,
                  !isReplicaSetEndpointActive /* requiresCollectionToExist */);

    assert.commandWorked(testDb.createCollection(testCollName1));

    runTestForCmd(adminDb,
                  makeTestConfigureAnalyzerCmdObj,
                  testDbName,
                  testCollName0,
                  true /* requiresCollectionToExist */);
    runTestForCmd(adminDb,
                  makeTestAnalyzeShardKeyCmdObj,
                  testDbName,
                  testCollName0,
                  true /* requiresCollectionToExist */);
    runTestForCmd(adminDb,
                  makeTestListSampledQueriesAggregateCmdObj,
                  testDbName,
                  testCollName0,
                  false /* requiresCollectionToExist */);
    runTestForCmd(primaryTestDb,
                  makeTestAnalyzeShardKeyAggregateCmdObj,
                  testDbName,
                  testCollName0,
                  true /* requiresCollectionToExist */);
}

{
    const st = new ShardingTest({shards: 1});
    runTests(st.s, st.rs0);
    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    runTests(primary, rst);
    rst.stopSet();
}
