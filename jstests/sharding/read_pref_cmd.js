if (0) { // SERVER-10429, SERVER-7533
load("jstests/replsets/rslib.js");

var NODE_COUNT = 2;

/**
 * Prepare to call testReadPreference() or assertFailure().
 */
var setUp = function() {
    var configDB = st.s.getDB('config');
    configDB.adminCommand({ enableSharding: 'test' });
    configDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

    // Each time we drop the 'test' DB we have to re-enable profiling
    st.rs0.nodes.forEach(function(node) {
        node.getDB('test').setProfilingLevel(2);
    });
};

/**
 * Clean up after testReadPreference() or testBadMode(), prepare to call setUp() again.
 */
var tearDown = function() {
    st.s.getDB('test').dropDatabase();
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
 * @param secExpected {boolean} true if we expect to run any commands on secondary
 */
var testReadPreference = function(conn, hostList, isMongos, mode, tagSets, secExpected) {
    var testDB = conn.getDB('test');
    conn.setSlaveOk(false); // purely rely on readPref
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
     */
    var cmdTest = function(cmdObj, secOk, profileQuery) {
        jsTest.log('about to do: ' + tojson(cmdObj));
        var cmdResult = testDB.runCommand(cmdObj);
        jsTest.log('cmd result: ' + tojson(cmdResult));
        assert(cmdResult.ok);

        var testedAtLeastOnce = false;
        var query = { op: 'command', ns: 'test.$cmd' };
        Object.extend(query, profileQuery);

        hostList.forEach(function(node) {
            var testDB = node.getDB('test');
            var result = testDB.system.profile.findOne(query);

            if (result != null) {
                if (secOk && secExpected) {
                    // The command obeys read prefs and we expect to run
                    // commands on secondaries with this mode and tag sets
                    assert(testDB.adminCommand({ isMaster: 1 }).secondary);
                }
                else {
                    // The command does not obey read prefs, or we expect to run
                    // commands on primary with this mode or tag sets
                    assert(testDB.adminCommand({ isMaster: 1 }).ismaster);
                }

                testedAtLeastOnce = true;
            }
        });

        assert(testedAtLeastOnce);
    };

    /**
     * Assumption: all values are native types (no objects)
     */
    var formatProfileQuery = function(queryObj, isEmbedded) {
        var prefix = isEmbedded? 'command.query.' : 'command.';
        var newObj = {};

        for (var field in queryObj) {
            newObj[prefix + field] = queryObj[field];
        }

        return newObj;
    };

    // Test command that can be sent to secondary
    cmdTest({ distinct: 'user', key: { x: 1 }, query: { x: 1 }}, true,
        formatProfileQuery({ distinct: 'user' }, !isMongos));

    // Test command that can't be sent to secondary
    cmdTest({ create: 'mrIn' }, false, formatProfileQuery({ create: 'mrIn' }, !isMongos));
    // Make sure mrIn is propagated to secondaries before proceeding
    testDB.runCommand({ getLastError: 1, w: NODE_COUNT });

    var mapFunc = function(doc) {};
    var reduceFunc = function(key, values) { return values; };

    // Test inline mapReduce on sharded collection.
    // Note that in sharded map reduce, it will output the result in a temp collection
    // even if out is inline.
    if (isMongos) {
        cmdTest({ mapreduce: 'user', map: mapFunc, reduce: reduceFunc, out: { inline: 1 }},
            false, formatProfileQuery({ mapreduce: 'user', shardedFirstPass: true }, false));
    }

    // Test inline mapReduce on unsharded collection.
    cmdTest({ mapreduce: 'mrIn', map: mapFunc, reduce: reduceFunc, out: { inline: 1 }}, true,
        formatProfileQuery({ mapreduce: 'mrIn', 'out.inline': 1 }, !isMongos));

    // Test non-inline mapReduce on sharded collection.
    if (isMongos) {
        cmdTest({ mapreduce: 'user', map: mapFunc, reduce: reduceFunc,
            out: { replace: 'mrOut' }}, false,
            formatProfileQuery({ mapreduce: 'user', shardedFirstPass: true }, false));
    }

    // Test non-inline mapReduce on unsharded collection.
    cmdTest({ mapreduce: 'mrIn', map: mapFunc, reduce: reduceFunc, out: { replace: 'mrOut' }},
        false, formatProfileQuery({ mapreduce: 'mrIn', 'out.replace': 'mrOut' }, !isMongos));

    // Test other commands that can be sent to secondary.
    cmdTest({ count: 'user' }, true, formatProfileQuery({ count: 'user' }, !isMongos));
    cmdTest({ group: { key: { x: true }, '$reduce': function(a, b) {}, ns: 'mrIn',
        initial: { x: 0  }}}, true, formatProfileQuery({ 'group.ns': 'mrIn' }, !isMongos));

    cmdTest({ collStats: 'user' }, true, formatProfileQuery({ count: 'user' }, !isMongos));
    cmdTest({ dbStats: 1 }, true, formatProfileQuery({ dbStats: 1 }, !isMongos));

    testDB.user.ensureIndex({ loc: '2d' });
    testDB.user.ensureIndex({ position: 'geoHaystack', type:1 }, { bucketSize: 10 });
    testDB.runCommand({ getLastError: 1, w: NODE_COUNT });
    cmdTest({ geoNear: 'user', near: [1, 1] }, true,
        formatProfileQuery({ geoNear: 'user' }, !isMongos));

    // Mongos doesn't implement geoSearch; test it only with ReplicaSetConnection.
    // We'll omit geoWalk, it's not a public command.
    if (!isMongos) {
        cmdTest(
            {
                geoSearch: 'user', near: [1, 1],
                search: { type: 'restaurant'}, maxDistance: 10
            }, true, formatProfileQuery({ geoSearch: 'user'}, true));
    }

    // Test on sharded
    cmdTest({ aggregate: 'user', pipeline: [{ $project: { x: 1 }}] }, true,
        formatProfileQuery({ aggregate: 'user' }, !isMongos));

    // Test on non-sharded
    cmdTest({ aggregate: 'mrIn', pipeline: [{ $project: { x: 1 }}] }, true,
        formatProfileQuery({ aggregate: 'mrIn' }, !isMongos));
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
    conn.setReadPref(mode, tagSets);
    testDB = conn.getDB('test');

    // Test that a command that could be routed to a secondary fails with bad mode / tags.
    if (isMongos) {
        // Command result should have ok: 0.
        cmdResult = testDB.runCommand({ distinct: 'user', key: { x: 1 } });
        jsTest.log('cmd result: ' + tojson(cmdResult));
        assert(!cmdResult.ok);
    } else {
        try {
            // conn should throw error
            testDB.runCommand({ distinct: 'user', key: { x: 1 } });
            failureMsg = "Unexpected success running distinct!";
        }
        catch (e) {
            jsTest.log(e);
        }

        if (failureMsg) throw failureMsg;
    }

    // Can't be routed to secondary, succeeds by ignoring mode and tags
    testDB.runCommand({ create: 'quux' });
};

var testAllModes = function(conn, hostList, isMongos) {

    // The primary is tagged with { tag: 'one' } and the secondary with
    // { tag: 'two' } so we can test the interaction of modes and tags. Test
    // a bunch of combinations.
    [
        // mode, tagSets, expectedHost
        ['primary', undefined, false],
        ['primary', [{}], false],

        ['primaryPreferred', undefined, false],
        ['primaryPreferred', [{tag: 'one'}], false],
        // Correctly uses primary and ignores the tag
        ['primaryPreferred', [{tag: 'two'}], false],

        ['secondary', undefined, true],
        ['secondary', [{tag: 'two'}], true],
        ['secondary', [{tag: 'doesntexist'}, {}], true],
        ['secondary', [{tag: 'doesntexist'}, {tag:'two'}], true],

        ['secondaryPreferred', undefined, true],
        ['secondaryPreferred', [{tag: 'one'}], false],
        ['secondaryPreferred', [{tag: 'two'}], true],

        // We don't have a way to alter ping times so we can't predict where an
        // untagged 'nearest' command should go, hence only test with tags.
        ['nearest', [{tag: 'one'}], false],
        ['nearest', [{tag: 'two'}], true]

    ].forEach(function(args) {
        var mode = args[0], tagSets = args[1], secExpected = args[2];

        setUp();
        testReadPreference(conn, hostList, isMongos, mode, tagSets, secExpected);
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

var st = new ShardingTest({shards : {rs0 : {nodes : NODE_COUNT, verbose : 1}},
                           other : {mongosOptions : {verbose : 3}}});
st.stopBalancer();

ReplSetTest.awaitRSClientHosts(st.s, st.rs0.nodes);

// Tag primary with { dc: 'ny', tag: 'one' }, secondary with { dc: 'ny', tag: 'two' }
var primary = st.rs0.getPrimary();
var secondary = st.rs0.getSecondary();
var rsConfig = primary.getDB("local").system.replset.findOne();
jsTest.log('got rsconf ' + tojson(rsConfig));
rsConfig.members.forEach(function(member) {
    if (member.host == primary.host) {
        member.tags = { dc: 'ny', tag: 'one' }
    } else {
        member.tags = { dc: 'ny', tag: 'two' }
    }
});

rsConfig.version++;


jsTest.log('new rsconf ' + tojson(rsConfig));

try {
    primary.adminCommand({ replSetReconfig: rsConfig });
}
catch(e) {
    jsTest.log('replSetReconfig error: ' + e);
}

st.rs0.awaitSecondaryNodes();

// Force mongos to reconnect after our reconfig
assert.soon(function() {
    try {
        st.s.getDB('foo').runCommand({ create: 'foo' });
        return true;
    }
    catch (x) {
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

// TODO: use api in SERVER-7533 once available.
// Make sure replica set connection is ready by repeatedly performing a dummy query
// against the secondary until it succeeds. This hack is needed because awaitRSClientHosts
// won't work on the shell's instance of the ReplicaSetMonitor.
assert.soon(function() {
    try {
        replConn.getDB('test').user.find().readPref('secondary').hasNext();
        return true;
    }
    catch (x) {
        // Intentionally caused an error that forces the monitor to refresh.
        print('Caught exception while doing dummy query: ' + tojson(x));
        return false;
    }
});

testAllModes(replConn, st.rs0.nodes, false);

jsTest.log('Starting test for mongos connection');

testAllModes(st.s, st.rs0.nodes, true);

st.stop();

}
