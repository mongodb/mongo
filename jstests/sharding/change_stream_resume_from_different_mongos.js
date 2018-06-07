// Test resuming a change stream on a mongos other than the one the change stream was started on.
(function() {
    "use strict";
    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const st = new ShardingTest({
        shards: 2,
        mongos: 2,
        rs: {nodes: 3, setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}}
    });

    for (let key of Object.keys(ChangeStreamWatchMode)) {
        const watchMode = ChangeStreamWatchMode[key];
        jsTestLog("Running test for mode " + watchMode);

        const s0DB = st.s0.getDB("test");
        const s1DB = st.s1.getDB("test");
        const coll = assertDropAndRecreateCollection(s0DB, "change_stream_failover");

        const nDocs = 100;

        // Split so ids < nDocs / 2 are for one shard, ids >= nDocs / 2 + 1 for another.
        st.shardColl(coll,
                     {_id: 1},              // key
                     {_id: nDocs / 2},      // split
                     {_id: nDocs / 2 + 1},  // move
                     "test",                // dbName
                     false                  // waitForDelete
                     );

        // Open a change stream.
        const cst = new ChangeStreamTest(ChangeStreamTest.getDBForChangeStream(watchMode, s0DB));
        let changeStream = cst.getChangeStream({watchMode: watchMode, coll: coll});

        // Be sure we can read from the change stream. Write some documents that will end up on
        // each shard. Use a bulk write to increase the chance that two of the writes get the same
        // cluster time on each shard.
        const kIds = [];
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < nDocs / 2; i++) {
            // Interleave elements which will end up on shard 0 with elements that will end up on
            // shard 1.
            kIds.push(i);
            bulk.insert({_id: i});
            kIds.push(i + nDocs / 2);
            bulk.insert({_id: i + nDocs / 2});
        }
        assert.commandWorked(bulk.execute());

        // Read from the change stream. The order of the documents isn't guaranteed because we
        // performed a bulk write.
        const firstChange = cst.getOneChange(changeStream);
        const docsFoundInOrder = [firstChange];
        for (let i = 0; i < nDocs - 1; i++) {
            const change = cst.getOneChange(changeStream);
            assert.docEq(change.ns, {db: s0DB.getName(), coll: coll.getName()});
            assert.eq(change.operationType, "insert");

            docsFoundInOrder.push(change);
        }

        // Assert that we found the documents we inserted (in any order).
        assert.setEq(new Set(kIds), new Set(docsFoundInOrder.map(doc => doc.fullDocument._id)));
        cst.cleanUp();

        // Now resume using the resume token from the first change on a different mongos.
        const otherCst =
            new ChangeStreamTest(ChangeStreamTest.getDBForChangeStream(watchMode, s1DB));

        const resumeCursor = otherCst.getChangeStream(
            {watchMode: watchMode, coll: coll, resumeAfter: firstChange._id});

        // Get the resume tokens for each change that occurred.
        const resumeTokens = [firstChange._id];
        for (let i = 0; i < kIds.length - 1; i++) {
            resumeTokens.push(otherCst.getOneChange(resumeCursor)._id);
        }

        // Check that resuming from each possible resume token works.
        for (let i = 0; i < resumeTokens.length; i++) {
            const cursor = otherCst.getChangeStream(
                {watchMode: watchMode, coll: coll, resumeAfter: resumeTokens[i]});
            otherCst.assertNextChangesEqual(
                {cursor: cursor, expectedChanges: docsFoundInOrder.splice(i + 1)});
        }
        otherCst.cleanUp();
    }

    st.stop();
}());
