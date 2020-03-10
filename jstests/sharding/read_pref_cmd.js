/**
 * This test is labeled resource intensive because its total io_write is 47MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [
 *   resource_intensive,
 *   requires_fcv_44,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

const nodeCount = 3;
const kDbName = "read_pref_cmd";

const kShardedCollName = "testCollSharded";
const kShardedNs = kDbName + "." + kShardedCollName;
const kNumDocs = 10;

const kUnshardedCollName = "testCollUnsharded";
const kUnshardedNs = kDbName + "." + kUnshardedCollName;

const allowedOnSecondary = Object.freeze({kNever: 0, kAlways: 1});

// Checking UUID and index consistency involves reading from the config server through mongos, but
// this test sets an invalid readPreference on the connection to the mongos.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

/**
 * Prepares to call testConnReadPreference(), testCursorReadPreference() or testBadMode().
 */
var setUp = function() {
    var configDB = st.s.getDB('config');
    assert.commandWorked(configDB.adminCommand({enableSharding: kDbName}));
    assert.commandWorked(configDB.adminCommand({shardCollection: kShardedNs, key: {x: 1}}));

    // Each time we drop the database we have to re-enable profiling. Enable profiling on 'admin'
    // to test the $currentOp aggregation stage.
    st.rs0.nodes.forEach(function(node) {
        assert(node.getDB(kDbName).setProfilingLevel(2));
        assert(node.getDB('admin').setProfilingLevel(2));
    });
};

/**
 * Cleans up after testConnReadPreference(), testCursorReadPreference() or testBadMode(),
 * prepares to call setUp() again.
 */
var tearDown = function() {
    assert.commandWorked(st.s.getDB(kDbName).dropDatabase());
    // Hack until SERVER-7739 gets fixed
    st.rs0.awaitReplication();
};

/**
 * Returns a profile query for the given namespace and command query. Assumes that all values
 * are native types (no objects).
 */
let formatProfileQuery = function(ns, cmdQuery, isQueryOp = false) {
    let profileQuery = {op: isQueryOp ? "query" : "command", errCode: {$exists: false}};
    if (ns) {
        profileQuery["ns"] = ns;
    }

    for (const field in cmdQuery) {
        profileQuery["command." + field] = cmdQuery[field];
    }

    return profileQuery;
};

/**
 * Returns the serverStatus hedgingMetrics for the given mongos connection.
 */
let getHedgingMetrics = function(mongosConn) {
    return assert.commandWorked(mongosConn.adminCommand({serverStatus: 1})).hedgingMetrics;
};

/**
 * Returns true if hedging is expected for the command with the given hedge options
 * and properties.
 */
let isHedgingExpected = function(isMongos, hedgeOptions, secOk, isReadOnlyCmd) {
    return isMongos && isReadOnlyCmd && hedgeOptions && hedgeOptions.enabled && secOk;
};

/**
 * Returns the number of nodes in 'rsNodes' that ran the command that matches the given
 * 'profileQuery' to completion. If 'expectedNode' is "primary" or "secondary" (and 'secOk'
 * is true), checks that the command only ran on the specified node.
 */
let getNumNodesCmdRanOn = function(rsNodes, {dbName, profileQuery, expectedNode, secOk}) {
    let numNodesCmdRanOn = 0;
    rsNodes.forEach(function(node) {
        let profileDB = node.getDB(dbName);
        let result = profileDB.system.profile.findOne(profileQuery);

        if (result != null) {
            if (secOk && expectedNode == "secondary") {
                assert(profileDB.adminCommand({isMaster: 1}).secondary);
            } else if (expectedNode == "primary") {
                assert(profileDB.adminCommand({isMaster: 1}).ismaster);
            }
            numNodesCmdRanOn += 1;
        }
    });
    return numNodesCmdRanOn;
};

/**
 * Runs the given cmdFunc to run a command and asserts that the command runs successfully
 * on the node(s) that match the given read preference and expected node.
 */
let assertCmdRanOnExpectedNodes = function(conn, isMongos, rsNodes, cmdTestCase) {
    const hedgingMetricsBefore = isMongos ? getHedgingMetrics(conn) : {};
    cmdTestCase.cmdFunc();
    let hedgingMetricsAfter = isMongos ? getHedgingMetrics(conn) : {};

    if (cmdTestCase.expectHedging) {
        const numOperations =
            hedgingMetricsAfter.numTotalOperations - hedgingMetricsBefore.numTotalOperations;
        const numHedgedOperations = hedgingMetricsAfter.numTotalHedgedOperations -
            hedgingMetricsBefore.numTotalHedgedOperations;

        assert.eq(numOperations, 1, "expect the command to be eligible for hedging");
        if (numHedgedOperations == 0) {
            // We did not hedge the operation That is, we did not manage to acquire a connection
            // to one other eligible node and send out an additional request before the command
            // finished.
            assert.eq(1, getNumNodesCmdRanOn(rsNodes, cmdTestCase));
            return;
        }

        // We did hedge the operation. That is, we did acquire a connection to one other eligible
        // node and try to send an additional request. So if the request had already been sent
        // when the command finished and the remote killOp did not occur quickly enough, that
        // other node could also run the command to completion.
        assert.eq(numHedgedOperations, 1);
        assert.gte(getNumNodesCmdRanOn(rsNodes, cmdTestCase), 1);
    } else {
        assert.eq(getNumNodesCmdRanOn(rsNodes, cmdTestCase), 1);
    }
};

/**
 * Sets the connection's read preference, performs a series of commands, and verifies that
 * each command runs on the expected node.
 *
 * @param conn {Mongo} the connection object of which to test the read preference functionality.
 * @param isMongos {boolean} true if conn is a mongos connection.
 * @param rsNodes {Array.<Mongo>} list of the replica set node members.
 * @param readPref {Object} object containing the following keys:
 *          mode {string} a read preference mode like "secondary".
 *          tagSets {Array.<Object>} list of tag sets to use.
 *          hedge {Object} hedge options of the form {enabled: <bool>}.
 * @param expectedNode {string} which node should this run on: "primary", "secondary", or "any".
 */
let testConnReadPreference = function(conn, isMongos, rsNodes, {readPref, expectedNode}) {
    jsTest.log(`Testing ${isMongos ? "mongos" : "mongod"} connection with readPreference mode: ${
        readPref.mode}, tag sets: ${tojson(readPref.tagSets)}, hedge ${tojson(readPref.hedge)}`);

    const hedgingEnabled = readPref.hedge && readPref.hedge.enabled;

    let testDB = conn.getDB(kDbName);
    let shardedColl = conn.getCollection(kShardedNs);
    conn.setSlaveOk(false);  // purely rely on readPref
    conn.setReadPref(readPref.mode, readPref.tagSets, readPref.hedge);

    /**
     * Performs the command and checks whether the command was routed to the
     * appropriate node(s).
     *
     * @param cmdObj the cmd to send.
     * @param secOk true if command can be routed to a secondary.
     * @param isReadOnlyCmd true if command cannot trigger writes.
     * @param profileQuery the query to perform agains the profile collection to
     *     look for the cmd just sent.
     * @param dbName the name of the database against which to run the command,
     *     and to which the 'system.profile' entry for this command is written.
     */
    var cmdTest = function(cmdObj, secOk, isReadOnlyCmd, profileQuery, dbName = kDbName) {
        jsTest.log('about to do: ' + tojson(cmdObj));

        const expectHedging = isHedgingExpected(isMongos, readPref.hedge, secOk, isReadOnlyCmd);
        const cmdFunc = () => {
            // Use runReadCommand so that the cmdObj is modified with the readPreference.
            const cmdResult = conn.getDB(dbName).runReadCommand(cmdObj);
            jsTest.log('cmd result: ' + tojson(cmdResult));
            assert.commandWorked(cmdResult);
        };

        assertCmdRanOnExpectedNodes(
            conn,
            isMongos,
            rsNodes,
            {expectHedging, expectedNode, cmdFunc, secOk, profileQuery, dbName});
    };

    // Test command that can be sent to secondary
    cmdTest({distinct: kShardedCollName, key: 'x', query: {x: 1}},
            allowedOnSecondary.kAlways,
            true,
            formatProfileQuery(kShardedNs, {distinct: kShardedCollName}));

    // Test command that can't be sent to secondary
    cmdTest({create: kUnshardedCollName},
            allowedOnSecondary.kNever,
            false,
            formatProfileQuery(kUnshardedNs, {create: kUnshardedCollName}));
    // Make sure the unsharded collection is propagated to secondaries before proceeding.
    testDB.runCommand({getLastError: 1, w: nodeCount});

    var mapFunc = function(doc) {};
    var reduceFunc = function(key, values) {
        return values;
    };

    // Test inline mapReduce on sharded collection.
    if (isMongos) {
        const comment = 'mapReduce_inline_sharded_' + ObjectId();
        cmdTest({
            mapreduce: kShardedCollName,
            map: mapFunc,
            reduce: reduceFunc,
            out: {inline: 1},
            comment: comment
        },
                allowedOnSecondary.kAlways,
                false,
                formatProfileQuery(kShardedNs, {aggregate: kShardedCollName, comment: comment}));
    }

    // Test inline mapReduce on unsharded collection.
    if (isMongos) {
        const comment = 'mapReduce_inline_unsharded_' + ObjectId();
        cmdTest(
            {
                mapreduce: kUnshardedCollName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {inline: 1},
                comment: comment
            },
            allowedOnSecondary.kAlways,
            false,
            formatProfileQuery(kUnshardedNs, {aggregate: kUnshardedCollName, comment: comment}));
    } else {
        cmdTest({mapreduce: kUnshardedCollName, map: mapFunc, reduce: reduceFunc, out: {inline: 1}},
                allowedOnSecondary.kAlways,
                false,
                formatProfileQuery(kUnshardedNs, {mapreduce: kUnshardedCollName, 'out.inline': 1}));
    }

    // Test non-inline mapReduce on sharded collection.
    if (isMongos) {
        const comment = 'mapReduce_noninline_sharded_' + ObjectId();
        cmdTest({
            mapreduce: kShardedCollName,
            map: mapFunc,
            reduce: reduceFunc,
            out: {replace: 'mrOut'},
            comment: comment
        },
                allowedOnSecondary.kAlways,
                false,
                formatProfileQuery(kShardedNs, {aggregate: kShardedCollName, comment: comment}));
    }

    // Test non-inline mapReduce on unsharded collection.
    if (isMongos) {
        const comment = 'mapReduce_noninline_unsharded_' + ObjectId();
        cmdTest(
            {
                mapreduce: kUnshardedCollName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {replace: 'mrOut'},
                comment: comment
            },
            allowedOnSecondary.kAlways,
            false,
            formatProfileQuery(kUnshardedNs, {aggregate: kUnshardedCollName, comment: comment}));
    } else {
        cmdTest({
            mapreduce: kUnshardedCollName,
            map: mapFunc,
            reduce: reduceFunc,
            out: {replace: 'mrOut'}
        },
                allowedOnSecondary.kNever,
                false,
                formatProfileQuery(kUnshardedNs,
                                   {mapreduce: kUnshardedCollName, 'out.replace': 'mrOut'}));
    }

    // Test other commands that can be sent to secondary.
    cmdTest({count: kShardedCollName},
            allowedOnSecondary.kAlways,
            true,
            formatProfileQuery(kShardedNs, {count: kShardedCollName}));
    cmdTest({collStats: kShardedCollName},
            allowedOnSecondary.kAlways,
            true,
            formatProfileQuery(kShardedNs, {collStats: kShardedCollName}));
    cmdTest(
        {dbStats: 1}, allowedOnSecondary.kAlways, true, formatProfileQuery(kDbName, {dbStats: 1}));

    assert.commandWorked(shardedColl.ensureIndex({loc: '2d'}));
    assert.commandWorked(
        shardedColl.ensureIndex({position: 'geoHaystack', type: 1}, {bucketSize: 10}));

    // TODO: SERVER-38961 Remove when simultaneous index builds complete.
    // Run a no-op command and wait for it to be applied on secondaries. Due to the asynchronous
    // completion nature of indexes on secondaries, we can guarantee an index build is complete
    // on all secondaries once all secondaries have applied this collMod command.
    assert.commandWorked(testDB.runCommand({collMod: kShardedCollName}));
    assert.commandWorked(testDB.runCommand({getLastError: 1, w: nodeCount}));

    // Mongos doesn't implement geoSearch; test it only with ReplicaSetConnection.
    if (!isMongos) {
        cmdTest({
            geoSearch: kShardedCollName,
            near: [1, 1],
            search: {type: 'restaurant'},
            maxDistance: 10
        },
                allowedOnSecondary.kAlways,
                true,
                formatProfileQuery(kShardedNs, {geoSearch: kShardedCollName}));
    }

    // Test on sharded
    cmdTest({aggregate: kShardedCollName, pipeline: [{$project: {x: 1}}], cursor: {}},
            allowedOnSecondary.kAlways,
            false,
            formatProfileQuery(kShardedNs, {
                aggregate: kShardedCollName,
                pipeline: [isMongos ? {$project: {_id: true, x: true}} : {$project: {x: 1}}]
            }));

    // Test on non-sharded
    cmdTest({aggregate: kUnshardedCollName, pipeline: [{$project: {x: 1}}], cursor: {}},
            allowedOnSecondary.kAlways,
            false,
            formatProfileQuery(kUnshardedNs,
                               {aggregate: kUnshardedCollName, pipeline: [{$project: {x: 1}}]}));

    // Test $currentOp aggregation stage.
    if (!isMongos) {
        let curOpComment = 'agg_currentOp_' + ObjectId();

        // A $currentOp without any foreign namespaces takes no collection locks and will not be
        // profiled, so we add a dummy $lookup stage to force an entry in system.profile.
        cmdTest({
            aggregate: 1,
            pipeline: [
                {$currentOp: {}},
                {$lookup: {from: "dummy", localField: "dummy", foreignField: "dummy", as: "dummy"}}
            ],
            comment: curOpComment,
            cursor: {}
        },
                allowedOnSecondary.kAlways,
                false,
                formatProfileQuery(undefined, {comment: curOpComment}),
                "admin");
    }
};

/**
 * Creates a cursor with the given read preference and verifies that the 'find' command runs
 * on the expected node.
 *
 * @param conn {Mongo} the connection object of which to test the read preference functionality.
 * @param isMongos {boolean} true if conn is a mongos connection.
 * @param rsNodes {Array.<Mongo>} list of the replica set node members.
 * @param readPref {Object} object containing the following keys:
 *          mode {string} a read preference mode like "secondary".
 *          tagSets {Array.<Object>} list of tag sets to use.
 *          hedge {Object} hedge options of the form {enabled: <bool>}.
 * @param expectedNode {string} which node should this run on: "primary", "secondary", or "any".
 */
let testCursorReadPreference = function(conn, isMongos, rsNodes, {readPref, expectedNode}) {
    jsTest.log(`Testing cursor with readPreference mode: ${readPref.mode}, tag sets: ${
        tojson(readPref.tagSets)}, hedge ${tojson(readPref.hedge)}`);

    let testColl = conn.getCollection(kShardedNs);
    conn.setSlaveOk(false);  // purely rely on readPref

    let bulk = testColl.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumDocs; ++i) {
        bulk.insert({_id: i, x: i});
    }
    assert.commandWorked(bulk.execute());

    const expectHedging =
        isHedgingExpected(isMongos, readPref.hedge, allowedOnSecondary.kAlways, true);

    let cursor =
        testColl.find({x: {$gte: 0}}).readPref(readPref.mode, readPref.tagSets, readPref.hedge);
    const cmdFunc = () => cursor.toArray();
    const secOk = allowedOnSecondary.kAlways;

    const profileQuery =
        formatProfileQuery(kShardedNs, {find: kShardedCollName, filter: {x: {$gte: 0}}}, true);
    const dbName = kDbName;

    assertCmdRanOnExpectedNodes(
        conn,
        isMongos,
        rsNodes,
        {expectHedging, expectedNode, cmdFunc, secOk, profileQuery, dbName});
};

/**
 * Verifies that commands fail with the given combination of mode, tags, and hedge options
 * in 'readPref'.
 *
 * @param conn {Mongo} the connection object of which to test the read preference functionality.
 * @param isMongos {boolean} true if conn is a mongos connection.
 * @param rsNodes {Array.<Mongo>} list of the replica set host members.
 * @param readPref {Object} object containing the following keys:
 *          mode {string} a read preference mode like "secondary".
 *          tagSets {Array.<Object>} list of tag sets to use.
 *          hedge {Object} hedge options of the form {enabled: <bool>}.
 * @param expectedNode {string} which node should this run on: "primary", "secondary", or "any".
 */
let testBadMode = function(conn, isMongos, rsNodes, readPref) {
    jsTest.log(`Expecting failure for mode: ${readPref.mode}, tag sets: ${
        tojson(readPref.tagSets)}, hedge ${tojson(readPref.hedge)}`);
    // use setReadPrefUnsafe to bypass client-side validation
    conn._setReadPrefUnsafe(readPref.mode, readPref.tagSets, readPref.hedge);
    let testDB = conn.getDB(kDbName);

    // Test that a command that could be routed to a secondary fails with bad mode / tags.
    if (isMongos) {
        // Command result should have ok: 0.
        const cmdResult = testDB.runReadCommand({distinct: kShardedCollName, key: 'x'});
        jsTest.log('cmd result: ' + tojson(cmdResult));
        assert(!cmdResult.ok);
    } else {
        let failureMsg;

        try {
            // conn should throw error
            testDB.runReadCommand({distinct: kShardedCollName, key: 'x'});
            failureMsg = "Unexpected success running distinct!";
        } catch (e) {
            jsTest.log(e.toString());
        }

        if (failureMsg)
            throw failureMsg;
    }
};

var testAllModes = function(conn, rsNodes, isMongos) {
    // The primary is tagged with { tag: "one" } and one of the secondaries is
    // tagged with { tag: "two" }. We can use this to test the interaction between
    // modes, tags, and hedge options. Test a bunch of combinations.
    [
        // readPref and expectedNode.
        {readPref: {mode: "primary"}, expectedNode: "primary"},
        {readPref: {mode: "primary", tagSets: []}, expectedNode: "primary"},

        {readPref: {mode: "primaryPreferred"}, expectedNode: "any"},
        {readPref: {mode: "primaryPreferred", tagSets: [{tag: "one"}]}, expectedNode: "primary"},
        {readPref: {mode: "primaryPreferred", tagSets: [{tag: "two"}]}, expectedNode: "any"},
        {readPref: {mode: "primaryPreferred", hedge: {enabled: false}}, expectedNode: "any"},

        {readPref: {mode: "secondary"}, expectedNode: "secondary"},
        {readPref: {mode: "secondary", tagSets: [{tag: "two"}]}, expectedNode: "secondary"},
        {
            readPref: {mode: "secondary", tagSets: [{tag: "doesntexist"}, {}]},
            expectedNode: "secondary"
        },
        {
            readPref: {mode: "secondary", tagSets: [{tag: "doesntexist"}, {tag: "two"}]},
            expectedNode: "secondary"
        },
        {readPref: {mode: "secondary", hedge: {enabled: false}}, expectedNode: "secondary"},
        {readPref: {mode: "secondary", hedge: {enabled: true}}, expectedNode: "secondary"},

        {readPref: {mode: 'secondaryPreferred'}, expectedNode: "any"},
        {readPref: {mode: 'secondaryPreferred', tagSets: [{tag: "one"}]}, expectedNode: "primary"},
        {readPref: {mode: 'secondaryPreferred', tagSets: [{tag: "two"}]}, expectedNode: "any"},
        {readPref: {mode: 'secondaryPreferred', hedge: {enabled: false}}, expectedNode: "any"},
        {readPref: {mode: 'secondaryPreferred', hedge: {enabled: true}}, expectedNode: "any"},

        // We don't have a way to alter ping times so we can't predict where an
        // untagged "nearest" command should go, hence only test with tags.
        {readPref: {mode: "nearest", tagSets: [{tag: "one"}]}, expectedNode: "primary"},
        {readPref: {mode: "nearest", tagSets: [{tag: "two"}]}, expectedNode: "secondary"},
        {readPref: {mode: "nearest", hedge: {enabled: false}}, expectedNode: "any"},
        {readPref: {mode: "nearest", hedge: {enabled: true}}, expectedNode: "any"}

    ].forEach(function(testCase) {
        setUp();

        // Run testCursorReadPreference() first since testConnReadPreference() sets the connection's
        // read preference.
        testCursorReadPreference(conn, isMongos, rsNodes, testCase);
        testConnReadPreference(conn, isMongos, rsNodes, testCase);

        tearDown();
    });

    [
        // Tags are not allowed in mode "primary".
        {readPref: {mode: "primary", tagSets: [{dc: "doesntexist"}]}},
        {readPref: {mode: "primary", tagSets: [{dc: "ny"}]}},
        {readPref: {mode: "primary", tagSets: [{dc: "one"}]}},

        // Hedging is not allowed in mode "primary".
        {readPref: {mode: "primary", hedge: {enabled: true}}},

        // No matching node.
        {readPref: {mode: "secondary", tagSets: [{tag: "one"}]}},
        {readPref: {mode: "nearest", tagSets: [{tag: "doesntexist"}]}},

        // Invalid mode, tags, hedgeOptions.
        {readPref: {mode: "invalid-mode"}},
        {readPref: {mode: "secondary", tagSets: ["misformatted-tags"]}},
        {readPref: {mode: "nearest", hedge: {doesnotexist: true}}},

    ].forEach(function(testCase) {
        setUp();
        testBadMode(conn, isMongos, rsNodes, testCase.readPref);
        tearDown();
    });
};

let st = new ShardingTest({shards: {rs0: {nodes: nodeCount}}});
st.stopBalancer();

awaitRSClientHosts(st.s, st.rs0.nodes);

// Tag the primary and secondaries. Set node priorities to force the primary to never change
// during this test.
let primary = st.rs0.getPrimary();
let secondaries = st.rs0.getSecondaries();
let secondary1 = secondaries[0];
let secondary2 = secondaries[1];

const kPrimaryTag = {
    dc: "ny",
    tag: "one"
};
const kSecondaryTag1 = {
    dc: "ny",
    tag: "two"
};
const kSecondaryTag2 = {
    dc: "ny",
    tag: 'three'
};

var rsConfig = primary.getDB("local").system.replset.findOne();
jsTest.log('got rsconf ' + tojson(rsConfig));
rsConfig.members.forEach(function(member) {
    switch (member.host) {
        case primary.host:
            member.priority = 1;
            member.tags = kPrimaryTag;
            break;
        case secondary1.host:
            member.priority = 0;
            member.tags = kSecondaryTag1;
            break;
        case secondary2.host:
            member.priority = 0;
            member.tags = kSecondaryTag2;
            break;
        default:
            throw Error("unknown host name " + member.host);
    }
});

rsConfig.version++;

jsTest.log('new rsconf ' + tojson(rsConfig));

try {
    primary.adminCommand({replSetReconfig: rsConfig});
} catch (e) {
    jsTest.log('replSetReconfig error: ' + e);
}

st.rs0.awaitSecondaryNodes();

// Force mongos to reconnect after our reconfig
assert.soon(function() {
    try {
        st.s.getDB('foo').runCommand({create: 'foo'});
        return true;
    } catch (x) {
        // Intentionally caused an error that forces mongos's monitor to refresh.
        jsTest.log('Caught exception while doing dummy command: ' + tojson(x));
        return false;
    }
});

reconnect(primary);
reconnect(secondary1);
reconnect(secondary2);

rsConfig = primary.getDB("local").system.replset.findOne();
jsTest.log('got rsconf ' + tojson(rsConfig));

var replConn = new Mongo(st.rs0.getURL());

// Make sure replica set connection is ready
_awaitRSHostViaRSMonitor(primary.name, {ok: true, tags: kPrimaryTag}, st.rs0.name);
_awaitRSHostViaRSMonitor(secondary1.name, {ok: true, tags: kSecondaryTag1}, st.rs0.name);
_awaitRSHostViaRSMonitor(secondary2.name, {ok: true, tags: kSecondaryTag2}, st.rs0.name);

st.rs0.nodes.forEach(function(conn) {
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, logComponentVerbosity: {command: {verbosity: 1}}}));
});

assert.commandWorked(
    st.s.adminCommand({setParameter: 1, logComponentVerbosity: {network: {verbosity: 3}}}));

testAllModes(replConn, st.rs0.nodes, false);

jsTest.log('Starting test for mongos connection');

// Force the mongos's replica set monitors to always include all the eligible nodes.
const replicaSetMonitorProtocol =
    assert.commandWorked(st.s.adminCommand({getParameter: 1, replicaSetMonitorProtocol: 1}))
        .replicaSetMonitorProtocol;
let failPoint = configureFailPoint(st.s,
                                   replicaSetMonitorProtocol === "scanning"
                                       ? "scanningServerSelectorIgnoreLatencyWindow"
                                       : "sdamServerSelectorIgnoreLatencyWindow");
testAllModes(st.s, st.rs0.nodes, true);
failPoint.off();

st.stop();
})();
