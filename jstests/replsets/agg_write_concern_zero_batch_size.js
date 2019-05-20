// Tests that an aggregate sent with batchSize: 0 will still obey the write concern sent on the
// original request, even though the writes happen in the getMore.
(function() {
    "use strict";

    load("jstests/aggregation/extras/out_helpers.js");  // For withEachKindOfWriteStage.
    load("jstests/libs/write_concern_util.js");         // For [stop|restart]ServerReplication.

    // Start a replica set with two nodes: one with the default configuration and one with priority
    // zero to ensure we don't have any elections.
    const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();

    const testDB = rst.getPrimary().getDB("test");
    const source = testDB.agg_write_concern_zero_batch_size;
    const target = testDB.agg_write_concern_zero_batch_size_target;
    assert.commandWorked(source.insert([{_id: 0}, {_id: 1}, {_id: 2}]));

    withEachKindOfWriteStage(target, (stageSpec) => {
        assert.commandWorked(target.remove({}));

        // Start an aggregate cursor with a writing stage, but use batchSize: 0 to prevent any
        // writes from happening in this command.
        const response = assert.commandWorked(testDB.runCommand({
            aggregate: source.getName(),
            pipeline: [stageSpec],
            writeConcern: {w: 2, wtimeout: 100},
            cursor: {batchSize: 0}
        }));
        assert.neq(response.cursor.id, 0);

        stopServerReplication(rst.getSecondary());

        const getMoreResponse = assert.commandFailedWithCode(
            testDB.runCommand({getMore: response.cursor.id, collection: source.getName()}),
            ErrorCodes.WriteConcernFailed);

        // Test the same thing but using the shell helpers.
        let error = assert.throws(
            () => source
                      .aggregate([stageSpec],
                                 {cursor: {batchSize: 0}, writeConcern: {w: 2, wtimeout: 100}})
                      .itcount());
        // Unfortunately this is the best way we have to check that the cause of the failure was due
        // to write concern. The aggregate shell helper will assert the command worked. When this
        // fails (as we expect due to write concern) it will create a new error object which loses
        // all structure and just preserves the information as text.
        assert(error instanceof Error);
        assert(tojson(error).indexOf("writeConcernError") != -1, tojson(error));

        // Now test without batchSize just to be sure.
        error = assert.throws(
            () => source.aggregate([stageSpec], {writeConcern: {w: 2, wtimeout: 100}}));
        assert(error instanceof Error);
        assert(tojson(error).indexOf("writeConcernError") != -1, tojson(error));

        // Now switch to legacy OP_GET_MORE read mode. We should get a different error indicating
        // that using writeConcern in this way is unsupported.
        source.getDB().getMongo().forceReadMode("legacy");
        error = assert.throws(
            () => source
                      .aggregate([stageSpec],
                                 {cursor: {batchSize: 0}, writeConcern: {w: 2, wtimeout: 100}})
                      .itcount());
        assert.eq(error.code, 31124);
        source.getDB().getMongo().forceReadMode("commands");

        restartServerReplication(rst.getSecondary());
    });

    rst.stopSet();
}());
