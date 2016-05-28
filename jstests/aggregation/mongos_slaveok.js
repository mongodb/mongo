/**
 * Tests aggregate command against mongos with slaveOk. For more tests on read preference,
 * please refer to jstests/sharding/read_pref_cmd.js.
 */
(function() {

    var NODES = 2;

    var doTest = function(st, doSharded) {
        var testDB = st.s.getDB('test');

        if (doSharded) {
            testDB.adminCommand({enableSharding: 'test'});
            testDB.adminCommand({shardCollection: 'test.user', key: {x: 1}});
        }

        testDB.user.insert({x: 10}, {writeConcern: {w: NODES}});
        testDB.setSlaveOk(true);

        var secNode = st.rs0.getSecondary();
        secNode.getDB('test').setProfilingLevel(2);

        // wait for mongos to recognize that the slave is up
        ReplSetTest.awaitRSClientHosts(st.s, secNode, {ok: true});

        var res = testDB.runCommand({aggregate: 'user', pipeline: [{$project: {x: 1}}]});
        assert(res.ok, 'aggregate command failed: ' + tojson(res));

        var profileQuery = {op: 'command', ns: 'test.user', 'command.aggregate': 'user'};
        var profileDoc = secNode.getDB('test').system.profile.findOne(profileQuery);

        assert(profileDoc != null);
        testDB.dropDatabase();
    };

    var st = new ShardingTest({shards: {rs0: {oplogSize: 10, nodes: NODES}}});

    doTest(st, false);
    doTest(st, true);

    st.stop();

})();
