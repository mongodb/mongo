/**
 * This test is labeled resource intensive because its total io_write is 47MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [resource_intensive]
 */
load("jstests/replsets/rslib.js");

var NODE_COUNT = 2;

// Checking UUID consistency involves reading from the config server through mongos, but this test
// sets an invalid readPreference on the connection to the mongos.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

/**
 * Prepare to call testReadPreference() or assertFailure().
 */
var setUp = function() {
    var configDB = st.s.getDB('config');
    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.user', key: {x: 1}}));

    // Each time we drop the 'test' DB we have to re-enable profiling. Enable profiling on 'admin'
    // to test the $currentOp aggregation stage.
    st.rs0.nodes.forEach(function(node) {
        assert(node.getDB('test').setProfilingLevel(2));
        assert(node.getDB('admin').setProfilingLevel(2));
    });
};

/**
 * Clean up after testReadPreference() or testBadMode(), prepare to call setUp() again.
 */
var tearDown = function() {
    assert.commandWorked(st.s.getDB('test').dropDatabase());
    // Hack until SERVER-7739 gets fixed
    st.rs0.awaitReplication();
};

/**
 * Performs a series of tests on commands with read preference.
 *
 * @param conn {Mongo} the connection object of which to test the read
 *     preference functionality.
 * @param hostList {Array.<Mongo>} list of the replica set host members.
 * @param isMongos {boolean} true if conn is a mongos connection.
 * @param mode {string} a read preference mode like 'secondary'
 * @param tagSets {Array.<Object>} list of tag sets to use
 * @param expectedHost {string} which host should this run on: 'primary', 'secondary', or 'any'
 */
var testReadPreference = function(conn, hostList, isMongos, mode, tagSets, expectedHost) {
    var testDB = conn.getDB('test');
    var adminDB = conn.getDB('admin');
    conn.setSlaveOk(false);  // purely rely on readPref
    jsTest.log('Testing mode: ' + mode + ', tag sets: ' + tojson(tagSets));
    conn.setReadPref(mode, tagSets);

    /**
     * Performs the command and checks whether the command was routed to the
     * appropriate node.
     *
     * @param cmdObj the cmd to send.
     * @param secOk true if command should be routed to a secondary.
     * @param profileQuery the query to perform agains the profile collection to
     *     look for the cmd just sent.
     * @param dbName the name of the database against which to run the command,
     *     and to which the 'system.profile' entry for this command is written.
     */
    var cmdTest = function(cmdObj, secOk, profileQuery, dbName = "test") {
        jsTest.log('about to do: ' + tojson(cmdObj));

        let runCmdDB = conn.getDB(dbName);

        // use runReadCommand so that the cmdObj is modified with the readPreference
        // set on the connection.
        var cmdResult = runCmdDB.runReadCommand(cmdObj);
        jsTest.log('cmd result: ' + tojson(cmdResult));
        assert(cmdResult.ok);

        var testedAtLeastOnce = false;
        var query = {op: 'command'};
        Object.extend(query, profileQuery);

        hostList.forEach(function(node) {
            var profileDB = node.getDB(dbName);
            var result = profileDB.system.profile.findOne(query);

            if (result != null) {
                if (secOk && expectedHost == "secondary") {
                    // The command obeys read prefs and we expect to run
                    // commands on secondaries with this mode and tag sets
                    assert(profileDB.adminCommand({isMaster: 1}).secondary);
                } else if (expectedHost == "primary") {
                    // The command does not obey read prefs, or we expect to run
                    // commands on primary with this mode or tag sets
                    assert(profileDB.adminCommand({isMaster: 1}).ismaster);
                }

                testedAtLeastOnce = true;
            }
        });

        assert(testedAtLeastOnce);
    };

    /**
     * Assumption: all values are native types (no objects)
     */
    var formatProfileQuery = function(queryObj) {
        var newObj = {};

        for (var field in queryObj) {
            newObj['command.' + field] = queryObj[field];
        }

        return newObj;
    };

    // Test command that can be sent to secondary
    cmdTest(
        {distinct: 'user', key: 'x', query: {x: 1}}, true, formatProfileQuery({distinct: 'user'}));

    // Test command that can't be sent to secondary
    cmdTest({create: 'mrIn'}, false, formatProfileQuery({create: 'mrIn'}));
    // Make sure mrIn is propagated to secondaries before proceeding
    testDB.runCommand({getLastError: 1, w: NODE_COUNT});

    var mapFunc = function(doc) {};
    var reduceFunc = function(key, values) {
        return values;
    };

    // Test inline mapReduce on sharded collection.
    // Note that in sharded map reduce, it will output the result in a temp collection
    // even if out is inline.
    if (isMongos) {
        cmdTest({mapreduce: 'user', map: mapFunc, reduce: reduceFunc, out: {inline: 1}},
                false,
                formatProfileQuery({mapreduce: 'user', shardedFirstPass: true}));
    }

    // Test inline mapReduce on unsharded collection.
    cmdTest({mapreduce: 'mrIn', map: mapFunc, reduce: reduceFunc, out: {inline: 1}},
            true,
            formatProfileQuery({mapreduce: 'mrIn', 'out.inline': 1}));

    // Test non-inline mapReduce on sharded collection.
    if (isMongos) {
        cmdTest({mapreduce: 'user', map: mapFunc, reduce: reduceFunc, out: {replace: 'mrOut'}},
                false,
                formatProfileQuery({mapreduce: 'user', shardedFirstPass: true}));
    }

    // Test non-inline mapReduce on unsharded collection.
    cmdTest({mapreduce: 'mrIn', map: mapFunc, reduce: reduceFunc, out: {replace: 'mrOut'}},
            false,
            formatProfileQuery({mapreduce: 'mrIn', 'out.replace': 'mrOut'}));

    // Test other commands that can be sent to secondary.
    cmdTest({count: 'user'}, true, formatProfileQuery({count: 'user'}));
    cmdTest({collStats: 'user'}, true, formatProfileQuery({count: 'user'}));
    cmdTest({dbStats: 1}, true, formatProfileQuery({dbStats: 1}));

    assert.commandWorked(testDB.user.ensureIndex({loc: '2d'}));
    assert.commandWorked(
        testDB.user.ensureIndex({position: 'geoHaystack', type: 1}, {bucketSize: 10}));

    // TODO: SERVER-38961 Remove when simultaneous index builds complete.
    // Run a no-op command and wait for it to be applied on secondaries. Due to the asynchronous
    // completion nature of indexes on secondaries, we can guarantee an index build is complete
    // on all secondaries once all secondaries have applied this collMod command.
    assert.commandWorked(testDB.runCommand({collMod: 'user'}));
    assert.commandWorked(testDB.runCommand({getLastError: 1, w: NODE_COUNT}));

    // Mongos doesn't implement geoSearch; test it only with ReplicaSetConnection.
    if (!isMongos) {
        cmdTest({geoSearch: 'user', near: [1, 1], search: {type: 'restaurant'}, maxDistance: 10},
                true,
                formatProfileQuery({geoSearch: 'user'}));
    }

    // Test on sharded
    cmdTest({aggregate: 'user', pipeline: [{$project: {x: 1}}], cursor: {}},
            true,
            formatProfileQuery({aggregate: 'user'}));

    // Test on non-sharded
    cmdTest({aggregate: 'mrIn', pipeline: [{$project: {x: 1}}], cursor: {}},
            true,
            formatProfileQuery({aggregate: 'mrIn'}));

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
                true,
                formatProfileQuery({comment: curOpComment}),
                "admin");
    }
};

/**
 * Verify that commands fail with the given combination of mode and tags.
 *
 * @param conn {Mongo} the connection object of which to test the read
 *     preference functionality.
 * @param hostList {Array.<Mongo>} list of the replica set host members.
 * @param isMongos {boolean} true if conn is a mongos connection.
 * @param mode {string} a read preference mode like 'secondary'
 * @param tagSets {Array.<Object>} list of tag sets to use
 */
var testBadMode = function(conn, hostList, isMongos, mode, tagSets) {
    var failureMsg, testDB, cmdResult;

    jsTest.log('Expecting failure for mode: ' + mode + ', tag sets: ' + tojson(tagSets));
    // use setReadPrefUnsafe to bypass client-side validation
    conn._setReadPrefUnsafe(mode, tagSets);
    testDB = conn.getDB('test');

    // Test that a command that could be routed to a secondary fails with bad mode / tags.
    if (isMongos) {
        // Command result should have ok: 0.
        cmdResult = testDB.runReadCommand({distinct: 'user', key: 'x'});
        jsTest.log('cmd result: ' + tojson(cmdResult));
        assert(!cmdResult.ok);
    } else {
        try {
            // conn should throw error
            testDB.runReadCommand({distinct: 'user', key: 'x'});
            failureMsg = "Unexpected success running distinct!";
        } catch (e) {
            jsTest.log(e.toString());
        }

        if (failureMsg)
            throw failureMsg;
    }
};

var testAllModes = function(conn, hostList, isMongos) {
    // The primary is tagged with { tag: 'one' } and the secondary with
    // { tag: 'two' } so we can test the interaction of modes and tags. Test
    // a bunch of combinations.
    [
        // mode, tagSets, expectedHost
        ['primary', undefined, "primary"],
        ['primary', [], "primary"],

        ['primaryPreferred', undefined, "any"],
        ['primaryPreferred', [{tag: 'one'}], "primary"],
        // Correctly uses primary and ignores the tag
        ['primaryPreferred', [{tag: 'two'}], "any"],

        ['secondary', undefined, "secondary"],
        ['secondary', [{tag: 'two'}], "secondary"],
        ['secondary', [{tag: 'doesntexist'}, {}], "secondary"],
        ['secondary', [{tag: 'doesntexist'}, {tag: 'two'}], "secondary"],

        ['secondaryPreferred', undefined, "any"],
        ['secondaryPreferred', [{tag: 'one'}], "primary"],
        ['secondaryPreferred', [{tag: 'two'}], "any"],

        // We don't have a way to alter ping times so we can't predict where an
        // untagged 'nearest' command should go, hence only test with tags.
        ['nearest', [{tag: 'one'}], "any"],
        ['nearest', [{tag: 'two'}], "any"]

    ].forEach(function(args) {
        var mode = args[0], tagSets = args[1], expectedHost = args[2];

        setUp();
        testReadPreference(conn, hostList, isMongos, mode, tagSets, expectedHost);
        tearDown();
    });

    [
        // Tags not allowed with primary
        ['primary', [{dc: 'doesntexist'}]],
        ['primary', [{dc: 'ny'}]],
        ['primary', [{dc: 'one'}]],

        // No matching node
        ['secondary', [{tag: 'one'}]],
        ['nearest', [{tag: 'doesntexist'}]],

        ['invalid-mode', undefined],
        ['secondary', ['misformatted-tags']]

    ].forEach(function(args) {
        var mode = args[0], tagSets = args[1];

        setUp();
        testBadMode(conn, hostList, isMongos, mode, tagSets);
        tearDown();
    });
};

var st = new ShardingTest({shards: {rs0: {nodes: NODE_COUNT}}});
st.stopBalancer();

awaitRSClientHosts(st.s, st.rs0.nodes);

// Tag primary with { dc: 'ny', tag: 'one' }, secondary with { dc: 'ny', tag: 'two' }
var primary = st.rs0.getPrimary();
var secondary = st.rs0.getSecondary();
var PRIMARY_TAG = {dc: 'ny', tag: 'one'};
var SECONDARY_TAG = {dc: 'ny', tag: 'two'};

var rsConfig = primary.getDB("local").system.replset.findOne();
jsTest.log('got rsconf ' + tojson(rsConfig));
rsConfig.members.forEach(function(member) {
    if (member.host == primary.host) {
        member.tags = PRIMARY_TAG;
    } else {
        member.tags = SECONDARY_TAG;
        member.priority = 0;
        member.votes = 0;
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
reconnect(secondary);

rsConfig = primary.getDB("local").system.replset.findOne();
jsTest.log('got rsconf ' + tojson(rsConfig));

var replConn = new Mongo(st.rs0.getURL());

// Make sure replica set connection is ready
_awaitRSHostViaRSMonitor(primary.name, {ok: true, tags: PRIMARY_TAG}, st.rs0.name);
_awaitRSHostViaRSMonitor(secondary.name, {ok: true, tags: SECONDARY_TAG}, st.rs0.name);

st.rs0.nodes.forEach(function(conn) {
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, logComponentVerbosity: {command: {verbosity: 1}}}));
});

assert.commandWorked(
    st.s.adminCommand({setParameter: 1, logComponentVerbosity: {network: {verbosity: 3}}}));

testAllModes(replConn, st.rs0.nodes, false);

jsTest.log('Starting test for mongos connection');

testAllModes(st.s, st.rs0.nodes, true);

st.stop();
