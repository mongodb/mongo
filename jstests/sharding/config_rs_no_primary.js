// Tests operation of the cluster when the config servers have no primary and thus the cluster
// metadata is in read-only mode.

// Checking UUID consistency involves talking to the config server primary, but there is no config
// server primary by the end of this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

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

    jsTestLog("Starting a new merizos when the config servers have no primary which should work");
    var merizos2 = MerizoRunner.runMerizos({configdb: st.configRS.getURL()});
    assert.neq(null, merizos2);

    var testOps = function(merizos) {
        jsTestLog("Doing ops that don't require metadata writes and thus should succeed against: " +
                  merizos);
        var initialCount = merizos.getDB('test').foo.count();
        assert.writeOK(merizos.getDB('test').foo.insert({a: 1}));
        assert.eq(initialCount + 1, merizos.getDB('test').foo.count());

        assert.throws(function() {
            merizos.getDB('config').shards.findOne();
        });
        merizos.setSlaveOk(true);
        var shardDoc = merizos.getDB('config').shards.findOne();
        merizos.setSlaveOk(false);
        assert.neq(null, shardDoc);

        jsTestLog("Doing ops that require metadata writes and thus should fail against: " + merizos);
        assert.writeError(merizos.getDB("newDB").foo.insert({a: 1}));
        assert.commandFailed(
            merizos.getDB('admin').runCommand({shardCollection: "test.foo", key: {a: 1}}));
    };

    testOps(merizos2);
    testOps(st.s);

    st.stop();
    MerizoRunner.stopMerizos(merizos2);
}());
