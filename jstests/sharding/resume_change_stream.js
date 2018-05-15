// Tests resuming change streams on sharded collections.
// We need to use a readConcern in this test, which requires read commands.
// @tags: [requires_find_command]
(function() {
    "use strict";

    load('jstests/replsets/rslib.js');           // For getLatestOp.
    load('jstests/libs/change_stream_util.js');  // For ChangeStreamTest.

    // For supportsMajorityReadConcern.
    load('jstests/multiVersion/libs/causal_consistency_helpers.js');

    // This test only works on storage engines that support committed reads, skip it if the
    // configured engine doesn't support it.
    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const oplogSize = 1;  // size in MB
    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 1,
            oplogSize: oplogSize,
            enableMajorityReadConcern: '',
            // Use the noop writer with a higher frequency for periodic noops to speed up the test.
            setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}
        }
    });

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];

    let cst = new ChangeStreamTest(mongosDB);

    function testResume(mongosColl, collToWatch) {
        mongosColl.drop();

        // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
        assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
        st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

        // Shard the test collection on _id.
        assert.commandWorked(
            mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

        // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
        assert.commandWorked(
            mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

        // Move the [0, MaxKey] chunk to st.shard1.shardName.
        assert.commandWorked(mongosDB.adminCommand(
            {moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

        // Write a document to each chunk.
        assert.writeOK(mongosColl.insert({_id: -1}, {writeConcern: {w: "majority"}}));
        assert.writeOK(mongosColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));

        let changeStream = cst.startWatchingChanges(
            {pipeline: [{$changeStream: {}}], collection: collToWatch, includeToken: true});

        // We awaited the replication of the first writes, so the change stream shouldn't return
        // them.
        assert.writeOK(mongosColl.update({_id: -1}, {$set: {updated: true}}));

        // Record current time to resume a change stream later in the test.
        const resumeTimeFirstUpdate = mongosDB.runCommand({isMaster: 1}).$clusterTime.clusterTime;

        assert.writeOK(mongosColl.update({_id: 1}, {$set: {updated: true}}));

        // Test that we see the two writes, and remember their resume tokens.
        let next = cst.getOneChange(changeStream);
        assert.eq(next.operationType, "update");
        assert.eq(next.documentKey._id, -1);
        const resumeTokenFromFirstUpdateOnShard0 = next._id;

        next = cst.getOneChange(changeStream);
        assert.eq(next.operationType, "update");
        assert.eq(next.documentKey._id, 1);
        const resumeTokenFromFirstUpdateOnShard1 = next._id;

        // Write some additional documents, then test that it's possible to resume after the first
        // update.
        assert.writeOK(mongosColl.insert({_id: -2}, {writeConcern: {w: "majority"}}));
        assert.writeOK(mongosColl.insert({_id: 2}, {writeConcern: {w: "majority"}}));

        changeStream = cst.startWatchingChanges({
            pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdateOnShard0}}],
            collection: collToWatch
        });

        for (let nextExpectedId of[1, -2, 2]) {
            assert.eq(cst.getOneChange(changeStream).documentKey._id, nextExpectedId);
        }

        // Test that the stream can't resume if the resume token is no longer present in the oplog.

        // Roll over the entire oplog on the shard with the resume token for the first update.
        const shardWithResumeToken = st.rs1.getPrimary();  // Resume from shard 1.
        const mostRecentOplogEntry = getLatestOp(shardWithResumeToken);
        assert.neq(mostRecentOplogEntry, null);
        const largeStr = new Array(4 * 1024 * oplogSize).join('abcdefghi');
        let i = 0;

        function oplogIsRolledOver() {
            // The oplog has rolled over if the op that used to be newest is now older than the
            // oplog's current oldest entry. Said another way, the oplog is rolled over when
            // everything in the oplog is newer than what used to be the newest entry.
            return bsonWoCompare(mostRecentOplogEntry.ts, getLeastRecentOp({
                                                              server: shardWithResumeToken,
                                                              readConcern: "majority"
                                                          }).ts) < 0;
        }

        while (!oplogIsRolledOver()) {
            let idVal = 100 + (i++);
            assert.writeOK(mongosColl.insert({_id: idVal, long_str: largeStr},
                                             {writeConcern: {w: "majority"}}));
            sleep(100);
        }

        ChangeStreamTest.assertChangeStreamThrowsCode({
            db: mongosDB,
            collName: collToWatch,
            pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdateOnShard1}}],
            expectedCode: 40576
        });

        ChangeStreamTest.assertChangeStreamThrowsCode({
            db: mongosDB,
            collName: collToWatch,
            pipeline: [{$changeStream: {startAtOperationTime: resumeTimeFirstUpdate}}],
            expectedCode: 40576
        });

        // Test that the change stream can't resume if the resume token *is* present in the oplog,
        // but one of the shards has rolled over its oplog enough that it doesn't have a long enough
        // history to resume. Since we just rolled over the oplog on shard 1, we know that
        // 'resumeTokenFromFirstUpdateOnShard0' is still present on shard 0, but shard 1 doesn't
        // have any changes earlier than that, so won't be able to resume.
        ChangeStreamTest.assertChangeStreamThrowsCode({
            db: mongosDB,
            collName: collToWatch,
            pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdateOnShard0}}],
            expectedCode: 40576
        });

        // Drop the collection.
        assert(mongosColl.drop());

        // Shard the test collection on shardKey.
        assert.commandWorked(
            mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {shardKey: 1}}));

        // Split the collection into 2 chunks: [MinKey, 50), [50, MaxKey].
        assert.commandWorked(
            mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {shardKey: 50}}));

        // Move the [50, MaxKey] chunk to st.shard1.shardName.
        assert.commandWorked(mongosDB.adminCommand(
            {moveChunk: mongosColl.getFullName(), find: {shardKey: 51}, to: st.rs1.getURL()}));

        const numberOfDocs = 100;

        // Insert test documents.
        for (let counter = 0; counter < numberOfDocs / 5; ++counter) {
            assert.writeOK(mongosColl.insert({_id: "abcd" + counter, shardKey: counter * 5 + 0},
                                             {writeConcern: {w: "majority"}}));
            assert.writeOK(mongosColl.insert({_id: "Abcd" + counter, shardKey: counter * 5 + 1},
                                             {writeConcern: {w: "majority"}}));
            assert.writeOK(mongosColl.insert({_id: "aBcd" + counter, shardKey: counter * 5 + 2},
                                             {writeConcern: {w: "majority"}}));
            assert.writeOK(mongosColl.insert({_id: "abCd" + counter, shardKey: counter * 5 + 3},
                                             {writeConcern: {w: "majority"}}));
            assert.writeOK(mongosColl.insert({_id: "abcD" + counter, shardKey: counter * 5 + 4},
                                             {writeConcern: {w: "majority"}}));
        }

        let allChangesCursor = cst.startWatchingChanges(
            {pipeline: [{$changeStream: {}}], collection: collToWatch, includeToken: true});

        // Perform the multi-update that will induce timestamp collisions
        assert.writeOK(mongosColl.update({}, {$set: {updated: true}}, {multi: true}));

        // Loop over documents and open inner change streams resuming from a specified position.
        // Note we skip the last document as it does not have the next document so we would
        // hang indefinitely.
        for (let counter = 0; counter < numberOfDocs - 1; ++counter) {
            let next = cst.getOneChange(allChangesCursor);

            const resumeToken = next._id;
            const caseInsensitive = {locale: "en_US", strength: 2};
            let resumedCaseInsensitiveCursor = cst.startWatchingChanges({
                pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
                collection: collToWatch,
                aggregateOptions: {collation: caseInsensitive}
            });
            cst.getOneChange(resumedCaseInsensitiveCursor);
        }
    }

    // Test change stream on a single collection.
    testResume(mongosColl, mongosColl.getName());

    // Test change stream on all collections.
    testResume(mongosColl, 1);

    cst.cleanUp();

    st.stop();
})();
