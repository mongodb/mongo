// Tests that a mongos on version 3.6 will be able to successfully operate a change stream within a
// session against shards that are binary version 4.0 but feature compatibility version 3.6.
(function() {
    "use strict";

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const st = new ShardingTest({
        shards: 2,
        mongosOptions: {binVersion: "3.6.7"},
        // Use replica sets to ensure we can use change streams, and make sure the periodic noop
        // writer is enabled so that we can depend on the cluster time advancing on each shard.
        rs: {nodes: 1, setParameter: {writePeriodicNoops: true}}
    });

    // Make sure the cluster starts up in a 3.6 compatibility version.
    const clusterFCV = assert.commandWorked(st.rs0.getPrimary().getDB("admin").runCommand(
        {getParameter: 1, featureCompatibilityVersion: 1}));
    assert.eq(clusterFCV.featureCompatibilityVersion.version, "3.6");

    const session = st.s0.getDB("test").getMongo().startSession();
    const mongosColl = session.getDatabase("test")[jsTestName()];

    // Shard the collection, split it into two chunks, and move the [1, MaxKey] chunk to the other
    // shard.
    st.shardColl(mongosColl, {_id: 1}, {_id: 1}, {_id: 1});
    const stream = mongosColl.watch();

    // Insert a document into one of the shards, but leave the other shard idle. This will force the
    // mongos to schedule getMores to the idle shard to make sure it is allowed to return this
    // insert and the idle shard will not return another event that should come before the insert.
    // Before SERVER-36212, these getMores to the idle shard would have caused the change stream to
    // abort due to a missing lsid.
    assert.commandWorked(mongosColl.insert({_id: 0}));

    assert.soon(() => stream.hasNext());
    assert.eq(stream.next().fullDocument, {_id: 0});

    stream.close();

    st.stop();
}());
