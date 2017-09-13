// Tests operation of the cluster when the config servers have no primary and thus the cluster
// metadata is in read-only mode.
(function() {
    "use strict";

    var st = new ShardingTest({
        shards: 1,
        other: {
            c0: {},  // Make sure 1st config server is primary
            c1: {rsConfig: {priority: 0}},
            c2: {rsConfig: {priority: 0}}
        }
    });

    assert.eq(st.config0, st.configRS.getPrimary());

    // Create the "test" database while the cluster metadata is still writeable.
    st.s.getDB('test').foo.insert({a: 1});

    // Take down two of the config servers so the remaining one goes into SECONDARY state.
    st.configRS.stop(1);
    st.configRS.stop(2);
    st.configRS.awaitNoPrimary();

    jsTestLog("Starting a new mongos when the config servers have no primary which should work");
    var mongos2 = MongoRunner.runMongos({configdb: st.configRS.getURL()});
    assert.neq(null, mongos2);

    var testOps = function(mongos) {
        jsTestLog("Doing ops that don't require metadata writes and thus should succeed against: " +
                  mongos);
        var initialCount = mongos.getDB('test').foo.count();
        assert.writeOK(mongos.getDB('test').foo.insert({a: 1}));
        assert.eq(initialCount + 1, mongos.getDB('test').foo.count());

        assert.throws(function() {
            mongos.getDB('config').shards.findOne();
        });
        mongos.setSlaveOk(true);
        var shardDoc = mongos.getDB('config').shards.findOne();
        mongos.setSlaveOk(false);
        assert.neq(null, shardDoc);

        jsTestLog("Doing ops that require metadata writes and thus should fail against: " + mongos);
        assert.writeError(mongos.getDB("newDB").foo.insert({a: 1}));
        assert.commandFailed(
            mongos.getDB('admin').runCommand({shardCollection: "test.foo", key: {a: 1}}));
    };

    testOps(mongos2);
    testOps(st.s);

    st.stop();
}());