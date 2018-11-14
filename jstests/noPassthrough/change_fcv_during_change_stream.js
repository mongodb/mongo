// Tests that a change stream's resume token format will adapt as the server's feature compatibility
// version changes.
// @tags: [requires_replication]
(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const testDB = primary.getDB("test");

    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

    const coll = testDB.change_fcv_during_change_stream;
    const changeStream36 = coll.watch([], {cursor: {batchSize: 1}});

    assert.commandWorked(coll.insert({_id: "first in 3.6"}));
    assert.commandWorked(coll.insert({_id: "second in 3.6"}));

    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    assert.commandWorked(coll.insert({_id: "first in 4.0"}));

    const session = testDB.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase("test");
    const sessionColl = sessionDb.change_fcv_during_change_stream;

    session.startTransaction();

    assert.commandWorked(sessionColl.insert({_id: "first in transaction"}));
    assert.commandWorked(sessionColl.insert({_id: "second in transaction"}));
    assert.commandWorked(sessionColl.remove({_id: "second in transaction"}));
    assert.commandWorked(sessionColl.insert({_id: "second in transaction"}));
    assert.commandWorked(sessionColl.insert({_id: "third in transaction"}));

    session.commitTransaction();
    session.endSession();

    function assertResumeTokenUsesStringFormat(resumeToken) {
        assert.neq(resumeToken._data, undefined);
        assert.eq(typeof resumeToken._data,
                  "string",
                  () => "Resume token: " + tojson(resumeToken) + ", oplog contents: " +
                      tojson(testDB.getSiblingDB("local").oplog.rs.find().toArray()));
    }

    function assertResumeTokenUsesBinDataFormat(resumeToken) {
        assert.neq(resumeToken._data, undefined);
        assert(resumeToken._data instanceof BinData, tojson(resumeToken));
    }

    // Start reading the change stream. The stream has been opened in 3.6 so make sure that the
    // resumeToken is in the appropriate format.
    assert.soon(() => changeStream36.hasNext());
    let nextChange = changeStream36.next();
    assert.eq(nextChange.documentKey._id, "first in 3.6");
    assertResumeTokenUsesBinDataFormat(nextChange._id);

    assert.soon(() => changeStream36.hasNext());
    nextChange = changeStream36.next();
    assert.eq(nextChange.documentKey._id, "second in 3.6");
    assertResumeTokenUsesBinDataFormat(nextChange._id);

    // At this point the server is switched to 4.0 and the resumeToken format should switch in 4.0
    // on the fly too.
    assert.soon(() => changeStream36.hasNext());
    nextChange = changeStream36.next();
    assert.eq(nextChange.documentKey._id, "first in 4.0");
    assertResumeTokenUsesStringFormat(nextChange._id);

    assert.soon(() => changeStream36.hasNext());
    nextChange = changeStream36.next();
    assert.eq(nextChange.documentKey._id, "first in transaction");
    assertResumeTokenUsesStringFormat(nextChange._id);

    assert.soon(() => changeStream36.hasNext());
    nextChange = changeStream36.next();
    assert.eq(nextChange.documentKey._id, "second in transaction");
    assertResumeTokenUsesStringFormat(nextChange._id);

    // Open a new change stream and position it in the middle of a transaction. Only the new 4.0
    // format resumeTokens are able to position correctly.
    const changeStream40 = coll.watch([], {resumeAfter: nextChange._id, cursor: {batchSize: 1}});
    assert.soon(() => changeStream40.hasNext());
    nextChange = changeStream40.next();
    assert.eq(nextChange.documentKey._id, "second in transaction");
    assertResumeTokenUsesStringFormat(nextChange._id);

    assert.soon(() => changeStream40.hasNext());
    nextChange = changeStream40.next();
    assert.eq(nextChange.documentKey._id, "second in transaction");
    assertResumeTokenUsesStringFormat(nextChange._id);

    assert.soon(() => changeStream40.hasNext());
    nextChange = changeStream40.next();
    assert.eq(nextChange.documentKey._id, "third in transaction");
    assertResumeTokenUsesStringFormat(nextChange._id);

    rst.stopSet();
}());
