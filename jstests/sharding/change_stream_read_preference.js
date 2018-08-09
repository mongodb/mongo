// Tests that change streams and their update lookups obey the read preference specified by the
// user.
// @tags: [requires_majority_read_concern]
(function() {
    "use strict";

    load('jstests/libs/profiler.js');  // For various profiler helpers.

    // For supportsMajorityReadConcern.
    load('jstests/multiVersion/libs/causal_consistency_helpers.js');

    // This test only works on storage engines that support committed reads, skip it if the
    // configured engine doesn't support it.
    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const st = new ShardingTest({
        name: "change_stream_read_pref",
        shards: 2,
        rs: {
            nodes: 2,
            // Use a higher frequency for periodic noops to speed up the test.
            setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}
        },
    });

    const dbName = jsTestName();
    const mongosDB = st.s0.getDB(dbName);
    const mongosColl = mongosDB[jsTestName()];

    // Enable sharding on the test DB and ensure its primary is shard0000.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard the test collection on _id.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

    // Move the [0, MaxKey] chunk to shard0001.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    // Turn on the profiler.
    for (let rs of[st.rs0, st.rs1]) {
        assert.commandWorked(rs.getPrimary().getDB(dbName).setProfilingLevel(2));
        assert.commandWorked(rs.getSecondary().getDB(dbName).setProfilingLevel(2));
    }

    // Write a document to each chunk.
    assert.writeOK(mongosColl.insert({_id: -1}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    // Test that change streams go to the primary by default.
    let changeStreamComment = "change stream against primary";
    const primaryStream = mongosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}],
                                               {comment: changeStreamComment});

    assert.writeOK(mongosColl.update({_id: -1}, {$set: {updated: true}}));
    assert.writeOK(mongosColl.update({_id: 1}, {$set: {updated: true}}));

    assert.soon(() => primaryStream.hasNext());
    assert.eq(primaryStream.next().fullDocument, {_id: -1, updated: true});
    assert.soon(() => primaryStream.hasNext());
    assert.eq(primaryStream.next().fullDocument, {_id: 1, updated: true});

    for (let rs of[st.rs0, st.rs1]) {
        const primaryDB = rs.getPrimary().getDB(dbName);
        // Test that the change stream itself goes to the primary. There might be more than one if
        // we needed multiple getMores to retrieve the changes.
        // TODO SERVER-31650 We have to use 'originatingCommand' here and look for the getMore
        // because the initial aggregate will not show up.
        profilerHasAtLeastOneMatchingEntryOrThrow(
            {profileDB: primaryDB, filter: {'originatingCommand.comment': changeStreamComment}});

        // Test that the update lookup goes to the primary as well.
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: primaryDB,
            filter: {
                op: "query",
                ns: mongosColl.getFullName(), "command.comment": changeStreamComment
            }
        });
    }

    primaryStream.close();

    // Test that change streams go to the secondary when the readPreference is {mode: "secondary"}.
    changeStreamComment = 'change stream against secondary';
    const secondaryStream =
        mongosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}],
                             {comment: changeStreamComment, $readPreference: {mode: "secondary"}});

    assert.writeOK(mongosColl.update({_id: -1}, {$set: {updatedCount: 2}}));
    assert.writeOK(mongosColl.update({_id: 1}, {$set: {updatedCount: 2}}));

    assert.soon(() => secondaryStream.hasNext());
    assert.eq(secondaryStream.next().fullDocument, {_id: -1, updated: true, updatedCount: 2});
    assert.soon(() => secondaryStream.hasNext());
    assert.eq(secondaryStream.next().fullDocument, {_id: 1, updated: true, updatedCount: 2});

    for (let rs of[st.rs0, st.rs1]) {
        const secondaryDB = rs.getSecondary().getDB(dbName);
        // Test that the change stream itself goes to the secondary. There might be more than one if
        // we needed multiple getMores to retrieve the changes.
        // TODO SERVER-31650 We have to use 'originatingCommand' here and look for the getMore
        // because the initial aggregate will not show up.
        profilerHasAtLeastOneMatchingEntryOrThrow(
            {profileDB: secondaryDB, filter: {'originatingCommand.comment': changeStreamComment}});

        // Test that the update lookup goes to the secondary as well.
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: secondaryDB,
            filter: {
                op: "query",
                ns: mongosColl.getFullName(), "command.comment": changeStreamComment,
                // We need to filter out any profiler entries with a stale config - this is the
                // first read on this secondary with a readConcern specified, so it is the first
                // read on this secondary that will enforce shard version.
                exceptionCode: {$ne: ErrorCodes.StaleConfig}
            }
        });
    }

    secondaryStream.close();
    st.stop();
}());
