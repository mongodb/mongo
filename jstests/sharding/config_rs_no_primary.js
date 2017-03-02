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

    jsTestLog("Starting a new bongos when the config servers have no primary which should work");
    var bongos2 = BongoRunner.runBongos({configdb: st.configRS.getURL()});
    assert.neq(null, bongos2);

    var testOps = function(bongos) {
        jsTestLog("Doing ops that don't require metadata writes and thus should succeed against: " +
                  bongos);
        var initialCount = bongos.getDB('test').foo.count();
        assert.writeOK(bongos.getDB('test').foo.insert({a: 1}));
        assert.eq(initialCount + 1, bongos.getDB('test').foo.count());

        assert.throws(function() {
            bongos.getDB('config').shards.findOne();
        });
        bongos.setSlaveOk(true);
        var shardDoc = bongos.getDB('config').shards.findOne();
        bongos.setSlaveOk(false);
        assert.neq(null, shardDoc);

        jsTestLog("Doing ops that require metadata writes and thus should fail against: " + bongos);
        assert.writeError(bongos.getDB("newDB").foo.insert({a: 1}));
        assert.commandFailed(
            bongos.getDB('admin').runCommand({shardCollection: "test.foo", key: {a: 1}}));
    };

    testOps(bongos2);
    testOps(st.s);

    st.stop();
}());