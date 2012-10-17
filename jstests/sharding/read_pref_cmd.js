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
    assert(createResult.ok);

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
    assert(inlineMRResult.ok);

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
    assert(outCollMRResult.ok);

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

doTest(new Mongo(st.rs0.getURL()), st.rs0.nodes);

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

