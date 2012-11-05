/**
 * Performs a series of test on commands with read preference
 *
 * @param conn {Mongo} the connection object of which to test the read
 *     preference functionality.
 * @param hostList {Array.<Mongo>} list of the replica set host members.
 */
var doTest = function(conn, hostList) {
    var testDB = conn.getDB('test');
    conn.setSlaveOk(false); // purely rely on readPref
    conn.setReadPref('secondary');

    // Test command that can be sent to secondary
    var distinctResult = testDB.runCommand({ distinct: 'user',
            key: { x: 1 }, query: { x: 1 }});

    var testedAtLeastOnce = false;
    hostList.forEach(function(node) {
        var testDB = node.getDB('test');
        var result = testDB.system.profile.findOne({ op: 'command',
            ns: 'test.$cmd', 'command.query.distinct': 'user' });

        if (result != null) {
            assert(testDB.adminCommand({ isMaster: 1 }).secondary);
            testedAtLeastOnce = true;
        }
    });

    assert(testedAtLeastOnce);

    // Test command that can't be sent to secondary
    var createResult = testDB.runCommand({ create: 'user' });
    assert(createResult.ok, 'create cmd failed: ' + tojson(createResult));

    testedAtLeastOnce = false;
    hostList.forEach(function(node) {
        var testDB = node.getDB('test');
        var result = testDB.system.profile.findOne({ op: 'command',
            ns: 'test.$cmd', 'command.query.create': 'user' });

        if (result != null) {
            assert(testDB.adminCommand({ isMaster: 1 }).ismaster);
            testedAtLeastOnce = true;
        }
    });

    assert(testedAtLeastOnce);

    // Test inline map reduce
    var mapFunc = function(doc) {};
    var reduceFunc = function(key, values) { return values; };

    var inlineMRResult = testDB.runCommand({ mapreduce: 'user', map: mapFunc,
        reduce: reduceFunc, out: { inline: 1 }});
    assert(inlineMRResult.ok, 'inline mr failed: ' + tojson(inlineMRResult));

    testedAtLeastOnce = false;
    hostList.forEach(function(node) {
        var testDB = node.getDB('test');
        var result = testDB.system.profile.findOne({ op: 'command',
            ns: 'test.$cmd', 'command.query.mapreduce': 'user' });

        if (result != null) {
            assert(testDB.adminCommand({ isMaster: 1 }).secondary);
            testedAtLeastOnce = true;
        }
    });

    // Test non-inline map reduce
    testDB.runCommand({ create: 'mrIn' });
    var outCollMRResult = testDB.runCommand({ mapreduce: 'mrIn', map: mapFunc,
        reduce: reduceFunc, out: { replace: 'mrOut' }});
    assert(outCollMRResult.ok, 'replace mr cmd failed: ' + tojson(outCollMRResult));

    testedAtLeastOnce = false;
    hostList.forEach(function(node) {
        var testDB = node.getDB('test');
        var result = testDB.system.profile.findOne({ op: 'command',
            ns: 'test.$cmd', 'command.query.mapreduce': 'mrIn' });

        if (result != null) {
            assert(testDB.adminCommand({ isMaster: 1 }).ismaster);
            testedAtLeastOnce = true;
        }
    });
};

var st = new ShardingTest({ shards: { rs0: { nodes: 2 }}});
st.stopBalancer();
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

doTest(replConn, st.rs0.nodes);

// TODO: uncomment once read preference command is properly implemented in mongos
/*
st.s.getDB('test').dropDatabase();
st.rs0.nodes.forEach(function(node) {
    node.getDB('test').setProfilingLevel(2);
});

jsTest.log('Starting test for mongos connection');

doTest(st.s, st.rs0.nodes);
*/

st.stop();

