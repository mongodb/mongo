/**
 * This test is labeled resource intensive because its total io_write is 47MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [
 *    resource_intensive,
 *    requires_scripting
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {awaitRSClientHosts, reconnect} from "jstests/replsets/rslib.js";

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
let setUp = function (rst) {
    let configDB = st.s.getDB("config");
    assert.commandWorked(configDB.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(configDB.adminCommand({shardCollection: kShardedNs, key: {x: 1}}));
    assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: kShardedNs}));

    // Each time we drop the database we have to re-enable profiling. Enable profiling on 'admin'
    // to test the $currentOp aggregation stage.
    rst.nodes.forEach(function (node) {
        assert(node.getDB(kDbName).setProfilingLevel(2));
        assert(node.getDB("admin").setProfilingLevel(2));
    });
};

/**
 * Cleans up after testConnReadPreference(), testCursorReadPreference() or testBadMode(),
 * prepares to call setUp() again.
 */
let tearDown = function (rst) {
    assert.commandWorked(st.s.getDB(kDbName).dropDatabase());
    rst.awaitReplication();
};

/**
 * Returns a profile query for the given namespace and command query. Assumes that all values
 * are native types (no objects).
 */
let formatProfileQuery = function (ns, cmdQuery, isQueryOp = false) {
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
 * Returns the number of nodes in 'rsNodes' that ran the command that matches the given
 * 'profileQuery' to completion. If 'expectedNode' is "primary" or "secondary" (and 'secOk'
 * is true), checks that the command only ran on the specified node.
 */
let getNumNodesCmdRanOn = function (rsNodes, {dbName, profileQuery, expectedNode, secOk}) {
    let numNodesCmdRanOn = 0;
    rsNodes.forEach(function (node) {
        let profileDB = node.getDB(dbName);
        let result = profileDB.system.profile.findOne(profileQuery);

        if (result != null) {
            if (secOk && expectedNode == "secondary") {
                assert(profileDB.adminCommand({hello: 1}).secondary);
            } else if (expectedNode == "primary") {
                assert(profileDB.adminCommand({hello: 1}).isWritablePrimary);
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
let assertCmdRanOnExpectedNodes = function (conn, isMongos, rsNodes, cmdTestCase) {
    cmdTestCase.cmdFunc();
    assert.eq(getNumNodesCmdRanOn(rsNodes, cmdTestCase), 1);
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
 * @param expectedNode {string} which node should this run on: "primary", "secondary", or "any".
 */
let testConnReadPreference = function (conn, isMongos, isReplicaSetEndpointActive, rst, {readPref, expectedNode}) {
    let rsNodes = rst.nodes;
    jsTest.log(
        `Testing ${isMongos ? "mongos" : "mongod"} connection with readPreference mode: ${
            readPref.mode
        }, tag sets: ${tojson(readPref.tagSets)}`,
    );

    let testDB = conn.getDB(kDbName);
    let shardedColl = conn.getCollection(kShardedNs);
    conn.setSecondaryOk(false); // purely rely on readPref
    conn.setReadPref(readPref.mode, readPref.tagSets);

    const isRouter = isMongos || isReplicaSetEndpointActive;

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
    let cmdTest = function (cmdObj, secOk, isReadOnlyCmd, profileQuery, dbName = kDbName) {
        jsTest.log("about to do: " + tojson(cmdObj));

        const cmdFunc = () => {
            // Use runReadCommand so that the cmdObj is modified with the readPreference.
            const cmdResult = conn.getDB(dbName).runReadCommand(cmdObj);
            jsTest.log("cmd result: " + tojson(cmdResult));
            assert.commandWorked(cmdResult);
        };

        assertCmdRanOnExpectedNodes(conn, isMongos, rsNodes, {expectedNode, cmdFunc, secOk, profileQuery, dbName});
    };

    // Test command that can be sent to secondary
    cmdTest(
        {distinct: kShardedCollName, key: "x", query: {x: 1}},
        allowedOnSecondary.kAlways,
        true,
        formatProfileQuery(kShardedNs, {distinct: kShardedCollName}),
    );

    // Test command that can't be sent to secondary
    cmdTest(
        {createIndexes: kUnshardedCollName, indexes: [{key: {x: 1}, name: "idx_x"}]},
        allowedOnSecondary.kNever,
        false,
        formatProfileQuery(kUnshardedNs, {createIndexes: kUnshardedCollName}),
    );

    // Make sure the unsharded collection is propagated to secondaries before proceeding.
    rst.awaitReplication();

    let mapFunc = function () {};
    let reduceFunc = function (key, values) {
        return values;
    };

    // Test inline mapReduce on sharded collection.
    if (isRouter) {
        const comment = "mapReduce_inline_sharded_" + ObjectId();
        cmdTest(
            {
                mapreduce: kShardedCollName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {inline: 1},
                comment: comment,
            },
            allowedOnSecondary.kAlways,
            false,
            formatProfileQuery(kShardedNs, {aggregate: kShardedCollName, comment: comment}),
        );
    }

    // Test inline mapReduce on unsharded collection.
    conn.getDB(kDbName).runCommand({create: kUnshardedCollName});
    if (isRouter) {
        const comment = "mapReduce_inline_unsharded_" + ObjectId();
        cmdTest(
            {
                mapreduce: kUnshardedCollName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {inline: 1},
                comment: comment,
            },
            allowedOnSecondary.kAlways,
            false,
            formatProfileQuery(kUnshardedNs, {aggregate: kUnshardedCollName, comment: comment}),
        );
    } else {
        cmdTest(
            {mapreduce: kUnshardedCollName, map: mapFunc, reduce: reduceFunc, out: {inline: 1}},
            allowedOnSecondary.kAlways,
            false,
            formatProfileQuery(kUnshardedNs, {mapreduce: kUnshardedCollName, "out.inline": 1}),
        );
    }

    // Test non-inline mapReduce on sharded collection.
    if (isRouter) {
        const comment = "mapReduce_noninline_sharded_" + ObjectId();
        cmdTest(
            {
                mapreduce: kShardedCollName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {replace: "mrOut_noninline_sharded"},
                comment: comment,
            },
            isMongos ? allowedOnSecondary.kAlways : allowedOnSecondary.kNever,
            false,
            formatProfileQuery(kShardedNs, {aggregate: kShardedCollName, comment: comment}),
        );
    }

    // Test non-inline mapReduce on unsharded collection.
    if (isRouter) {
        const comment = "mapReduce_noninline_unsharded_" + ObjectId();
        cmdTest(
            {
                mapreduce: kUnshardedCollName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {replace: "mrOut_noninline_unsharded"},
                comment: comment,
            },
            isMongos ? allowedOnSecondary.kAlways : allowedOnSecondary.kNever,
            false,
            formatProfileQuery(kUnshardedNs, {aggregate: kUnshardedCollName, comment: comment}),
        );
    } else {
        cmdTest(
            {
                mapreduce: kUnshardedCollName,
                map: mapFunc,
                reduce: reduceFunc,
                out: {replace: "mrOut_noninline_unsharded"},
            },
            allowedOnSecondary.kNever,
            false,
            formatProfileQuery(kUnshardedNs, {
                mapreduce: kUnshardedCollName,
                "out.replace": "mrOut_noninline_unsharded",
            }),
        );
    }

    // Test other commands that can be sent to secondary.
    cmdTest(
        {count: kShardedCollName},
        allowedOnSecondary.kAlways,
        true,
        formatProfileQuery(kShardedNs, {count: kShardedCollName}),
    );
    cmdTest(
        {collStats: kShardedCollName},
        allowedOnSecondary.kAlways,
        true,
        formatProfileQuery(kShardedNs, {collStats: kShardedCollName}),
    );
    cmdTest({dbStats: 1}, allowedOnSecondary.kAlways, true, formatProfileQuery(kDbName, {dbStats: 1}));

    assert.commandWorked(
        testDB.runCommand({
            createIndexes: shardedColl.getName(),
            indexes: [{key: {loc: "2d"}, name: "2d"}],
            writeConcern: {w: nodeCount},
        }),
    );

    // Test on sharded
    cmdTest(
        {aggregate: kShardedCollName, pipeline: [{$project: {x: 1}}], cursor: {}},
        allowedOnSecondary.kAlways,
        false,
        formatProfileQuery(kShardedNs, {
            aggregate: kShardedCollName,
            pipeline: [isRouter ? {$project: {_id: true, x: true}} : {$project: {x: 1}}],
        }),
    );

    const isMultiversion = Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet);

    const isValidMongos = !isMongos || MongoRunner.compareBinVersions(conn.fullOptions.binVersion, "7.1") >= 0;
    if (!isMultiversion && isValidMongos) {
        // Test on non-sharded. Skip testing in a multiversion scenario as the format of the
        // profiler entry will depend on the binary version of each shard as well as mongos.
        cmdTest(
            {aggregate: kUnshardedCollName, pipeline: [{$project: {x: 1}}], cursor: {}},
            allowedOnSecondary.kAlways,
            false,
            formatProfileQuery(kUnshardedNs, {
                aggregate: kUnshardedCollName,
                pipeline: [isRouter ? {$project: {_id: true, x: true}} : {$project: {x: 1}}],
            }),
        );
    }

    // Test $currentOp aggregation stage.
    if (!isRouter) {
        let curOpComment = "agg_currentOp_" + ObjectId();

        // A $currentOp without any foreign namespaces takes no collection locks and will not be
        // profiled, so we add a dummy $lookup stage to force an entry in system.profile.
        cmdTest(
            {
                aggregate: 1,
                pipeline: [
                    {$currentOp: {}},
                    {$lookup: {from: "dummy", localField: "dummy", foreignField: "dummy", as: "dummy"}},
                ],
                comment: curOpComment,
                cursor: {},
            },
            allowedOnSecondary.kAlways,
            false,
            formatProfileQuery(undefined, {comment: curOpComment}),
            "admin",
        );
    }

    if (!isMultiversion) {
        let curOpComment = "lockInfo_" + ObjectId();
        cmdTest(
            {lockInfo: 1, comment: curOpComment},
            allowedOnSecondary.kAlways,
            true,
            formatProfileQuery(undefined, {comment: curOpComment}),
            "admin",
        );
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
 * @param expectedNode {string} which node should this run on: "primary", "secondary", or "any".
 */
let testCursorReadPreference = function (conn, isMongos, rsNodes, {readPref, expectedNode}) {
    jsTest.log(`Testing cursor with readPreference mode: ${readPref.mode}, tag sets: ${tojson(readPref.tagSets)}`);

    let testColl = conn.getCollection(kShardedNs);
    conn.setSecondaryOk(false); // purely rely on readPref

    let bulk = testColl.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumDocs; ++i) {
        bulk.insert({_id: i, x: i});
    }
    assert.commandWorked(bulk.execute());

    if (isMongos) {
        // Do a read concern "local" read on each secondary so they refresh their metadata.
        testColl.find().readPref("secondary", [{tag: "two"}]);
        testColl.find().readPref("secondary", [{tag: "three"}]);
    }

    let cursor = testColl.find({x: {$gte: 0}}).readPref(readPref.mode, readPref.tagSets);
    const cmdFunc = () => cursor.toArray();
    const secOk = allowedOnSecondary.kAlways;

    const profileQuery = formatProfileQuery(kShardedNs, {find: kShardedCollName, filter: {x: {$gte: 0}}}, true);
    const dbName = kDbName;

    assertCmdRanOnExpectedNodes(conn, isMongos, rsNodes, {expectedNode, cmdFunc, secOk, profileQuery, dbName});
};

/**
 * Verifies that commands fail with the given combination of mode and tags
 * in 'readPref'.
 *
 * @param conn {Mongo} the connection object of which to test the read preference functionality.
 * @param isMongos {boolean} true if conn is a mongos connection.
 * @param rsNodes {Array.<Mongo>} list of the replica set host members.
 * @param readPref {Object} object containing the following keys:
 *          mode {string} a read preference mode like "secondary".
 *          tagSets {Array.<Object>} list of tag sets to use.
 * @param expectedNode {string} which node should this run on: "primary", "secondary", or "any".
 */
let testBadMode = function (conn, isMongos, rsNodes, readPref) {
    jsTest.log(`Expecting failure for mode: ${readPref.mode}, tag sets: ${tojson(readPref.tagSets)}`);
    // use setReadPrefUnsafe to bypass client-side validation
    conn._setReadPrefUnsafe(readPref.mode, readPref.tagSets);
    let testDB = conn.getDB(kDbName);

    // Test that a command that could be routed to a secondary fails with bad mode / tags.
    if (isMongos) {
        // Command result should have ok: 0.
        const cmdResult = testDB.runReadCommand({distinct: kShardedCollName, key: "x"});
        jsTest.log("cmd result: " + tojson(cmdResult));
        assert(!cmdResult.ok);
    } else {
        let failureMsg;

        try {
            // conn should throw error
            testDB.runReadCommand({distinct: kShardedCollName, key: "x"});
            failureMsg = "Unexpected success running distinct!";
        } catch (e) {
            jsTest.log(e.toString());
        }

        if (failureMsg) throw failureMsg;
    }
};

let testAllModes = function (conn, rst, isMongos, isReplicaSetEndpointActive) {
    // The primary is tagged with { tag: "one" } and one of the secondaries is
    // tagged with { tag: "two" }. We can use this to test the interaction between
    // modes and tags. Test a bunch of combinations.
    [
        // readPref and expectedNode.
        {readPref: {mode: "primary"}, expectedNode: "primary"},
        {readPref: {mode: "primary", tagSets: []}, expectedNode: "primary"},

        {readPref: {mode: "primaryPreferred"}, expectedNode: "any"},
        {readPref: {mode: "primaryPreferred", tagSets: [{tag: "one"}]}, expectedNode: "primary"},
        {readPref: {mode: "primaryPreferred", tagSets: [{tag: "two"}]}, expectedNode: "any"},
        {readPref: {mode: "primaryPreferred"}, expectedNode: "any"},

        {readPref: {mode: "secondary"}, expectedNode: "secondary"},
        {readPref: {mode: "secondary", tagSets: [{tag: "two"}]}, expectedNode: "secondary"},
        {
            readPref: {mode: "secondary", tagSets: [{tag: "doesntexist"}, {}]},
            expectedNode: "secondary",
        },
        {
            readPref: {mode: "secondary", tagSets: [{tag: "doesntexist"}, {tag: "two"}]},
            expectedNode: "secondary",
        },
        {readPref: {mode: "secondary"}, expectedNode: "secondary"},
        {readPref: {mode: "secondary"}, expectedNode: "secondary"},

        {readPref: {mode: "secondaryPreferred"}, expectedNode: "any"},
        {readPref: {mode: "secondaryPreferred", tagSets: [{tag: "one"}]}, expectedNode: "primary"},
        {readPref: {mode: "secondaryPreferred", tagSets: [{tag: "two"}]}, expectedNode: "any"},
        {readPref: {mode: "secondaryPreferred"}, expectedNode: "any"},
        {readPref: {mode: "secondaryPreferred"}, expectedNode: "any"},

        // We don't have a way to alter ping times so we can't predict where an
        // untagged "nearest" command should go, hence only test with tags.
        {readPref: {mode: "nearest", tagSets: [{tag: "one"}]}, expectedNode: "primary"},
        {readPref: {mode: "nearest", tagSets: [{tag: "two"}]}, expectedNode: "secondary"},
        {readPref: {mode: "nearest"}, expectedNode: "any"},
        {readPref: {mode: "nearest"}, expectedNode: "any"},
    ].forEach(function (testCase) {
        setUp(rst);

        // Run testCursorReadPreference() first since testConnReadPreference() sets the connection's
        // read preference.
        testCursorReadPreference(conn, isMongos, rst.nodes, testCase);
        testConnReadPreference(conn, isMongos, isReplicaSetEndpointActive, rst, testCase);

        tearDown(rst);
    });

    [
        // Tags are not allowed in mode "primary".
        {readPref: {mode: "primary", tagSets: [{dc: "doesntexist"}]}},
        {readPref: {mode: "primary", tagSets: [{dc: "ny"}]}},
        {readPref: {mode: "primary", tagSets: [{dc: "one"}]}},

        // No matching node.
        {readPref: {mode: "secondary", tagSets: [{tag: "one"}]}},
        {readPref: {mode: "nearest", tagSets: [{tag: "doesntexist"}]}},

        // Invalid mode, tags.
        {readPref: {mode: "invalid-mode"}},
        {readPref: {mode: "secondary", tagSets: ["misformatted-tags"]}},
    ].forEach(function (testCase) {
        setUp(rst);
        testBadMode(conn, isMongos, rst.nodes, testCase.readPref);
        tearDown(rst);
    });
};

let st = new ShardingTest({shards: {rs0: {nodes: nodeCount}}});
st.stopBalancer();
const isReplicaSetEndpointActive = st.isReplicaSetEndpointActive();

awaitRSClientHosts(st.s, st.rs0.nodes);

// Tag the primary and secondaries. Set node priorities to force the primary to never change
// during this test.
let primary = st.rs0.getPrimary();
let secondaries = st.rs0.getSecondaries();
let secondary1 = secondaries[0];
let secondary2 = secondaries[1];

const kPrimaryTag = {
    dc: "ny",
    tag: "one",
};
const kSecondaryTag1 = {
    dc: "ny",
    tag: "two",
};
const kSecondaryTag2 = {
    dc: "ny",
    tag: "three",
};

let rsConfig = primary.getDB("local").system.replset.findOne();
jsTest.log("got rsconf " + tojson(rsConfig));
rsConfig.members.forEach(function (member) {
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

jsTest.log("new rsconf " + tojson(rsConfig));

try {
    primary.adminCommand({replSetReconfig: rsConfig});
} catch (e) {
    jsTest.log("replSetReconfig error: " + e);
}

st.rs0.awaitSecondaryNodes();

// Force mongos to reconnect after our reconfig
assert.soon(function () {
    try {
        st.s.getDB("foo").runCommand({create: "foo"});
        return true;
    } catch (x) {
        // Intentionally caused an error that forces mongos's monitor to refresh.
        jsTest.log("Caught exception while doing dummy command: " + tojson(x));
        return false;
    }
});

reconnect(primary);
reconnect(secondary1);
reconnect(secondary2);

rsConfig = primary.getDB("local").system.replset.findOne();
jsTest.log("got rsconf " + tojson(rsConfig));

let replConn = new Mongo(st.rs0.getURL());

// Make sure replica set connection is ready
_awaitRSHostViaRSMonitor(primary.name, {ok: true, tags: kPrimaryTag}, st.rs0.name);
_awaitRSHostViaRSMonitor(secondary1.name, {ok: true, tags: kSecondaryTag1}, st.rs0.name);
_awaitRSHostViaRSMonitor(secondary2.name, {ok: true, tags: kSecondaryTag2}, st.rs0.name);

st.rs0.nodes.forEach(function (conn) {
    assert.commandWorked(conn.adminCommand({setParameter: 1, logComponentVerbosity: {command: {verbosity: 1}}}));
});

assert.commandWorked(st.s.adminCommand({setParameter: 1, logComponentVerbosity: {network: {verbosity: 3}}}));

testAllModes(replConn, st.rs0, false, isReplicaSetEndpointActive);

jsTest.log("Starting test for mongos connection");

// Force the mongos's replica set monitors to always include all the eligible nodes.
const replicaSetMonitorProtocol = assert.commandWorked(
    st.s.adminCommand({getParameter: 1, replicaSetMonitorProtocol: 1}),
).replicaSetMonitorProtocol;

assert(replicaSetMonitorProtocol === "streamable" || replicaSetMonitorProtocol === "sdam");

let failPoint = configureFailPoint(st.s, "sdamServerSelectorIgnoreLatencyWindow");

testAllModes(st.s, st.rs0, true, isReplicaSetEndpointActive);
failPoint.off();

st.stop();
