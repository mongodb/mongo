var NODE_COUNT = 2;

/**
 * Performs a series of tests on commands with read preference.
 *
 * @param conn {Mongo} the connection object of which to test the read
 *     preference functionality.
 * @param hostList {Array.<Mongo>} list of the replica set host members.
 * @param isMongos {boolean} true if conn is a mongos connection.
 */
var doTest = function(conn, hostList, isMongos) {
    var testDB = conn.getDB('test');
    conn.setSlaveOk(false); // purely rely on readPref
    conn.setReadPref('secondary');

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
        var cmdResult = testDB.runCommand(cmdObj, { readPref: { mode: 'secondary' }});
        jsTest.log('cmd result: ' + tojson(cmdResult));
        assert(cmdResult.ok);

        var testedAtLeastOnce = false;
        var query = { op: 'command', ns: 'test.$cmd' };
        Object.extend(query, profileQuery);

        hostList.forEach(function(node) {
            var testDB = node.getDB('test');
            var result = testDB.system.profile.findOne(query);

            if (result != null) {
                if (secOk) {
                    assert(testDB.adminCommand({ isMaster: 1 }).secondary);
                }
                else {
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
    testDB.runCommand({ getLastError: 1, w: NODE_COUNT });
    cmdTest({ geoNear: 'user', near: [1, 1] }, true,
        formatProfileQuery({ geoNear: 'user' }, !isMongos));

    // Test on sharded
    cmdTest({ aggregate: 'user', pipeline: [{ $project: { x: 1 }}] }, true,
        formatProfileQuery({ aggregate: 'user' }, !isMongos));

    // Test on non-sharded
    cmdTest({ aggregate: 'mrIn', pipeline: [{ $project: { x: 1 }}] }, true,
        formatProfileQuery({ aggregate: 'mrIn' }, !isMongos));
};

var st = new ShardingTest({ shards: { rs0: { nodes: NODE_COUNT }}});
st.stopBalancer();

var configDB = st.s.getDB('config');
configDB.adminCommand({ enableSharding: 'test' });
configDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

ReplSetTest.awaitRSClientHosts(st.s, st.rs0.nodes);

st.rs0.nodes.forEach(function(node) {
    node.getDB('test').setProfilingLevel(2);
});

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

doTest(new Mongo(st.rs0.getURL()), st.rs0.nodes, false);

st.s.getDB('test').dropDatabase();
// Hack until SERVER-7739 gets fixed
st.rs0.awaitReplication();

configDB.adminCommand({ enableSharding: 'test' });
configDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

st.rs0.nodes.forEach(function(node) {
    node.getDB('test').setProfilingLevel(2);
});

jsTest.log('Starting test for mongos connection');

doTest(st.s, st.rs0.nodes, true);

st.stop();

