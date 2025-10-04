// Tests that an aggregate sent with batchSize: 0 will still obey the write concern sent on the
// original request, even though the writes happen in the getMore.
import {withEachKindOfWriteStage} from "jstests/aggregation/extras/merge_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

// Start a replica set with two nodes: one with the default configuration and one with priority
// zero to ensure we don't have any elections.
const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    rst.getPrimary().adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

const testDB = rst.getPrimary().getDB("test");
const source = testDB.agg_write_concern_zero_batch_size;
const target = testDB.agg_write_concern_zero_batch_size_target;
assert.commandWorked(source.insert([{_id: 0}, {_id: 1}, {_id: 2}]));

// This test will cause commands to fail with writeConcern timeout. This normally triggers the
// hang analyzer, but we do not want to run it on expected timeouts. Thus we temporarily disable
// it.
MongoRunner.runHangAnalyzer.disable();

try {
    withEachKindOfWriteStage(target, (stageSpec) => {
        assert.commandWorked(target.remove({}));
        rst.awaitReplication();

        // Start an aggregate cursor with a writing stage, but use batchSize: 0 to prevent any
        // writes from happening in this command.
        const response = assert.commandWorked(
            testDB.runCommand({
                aggregate: source.getName(),
                pipeline: [stageSpec],
                writeConcern: {w: 2, wtimeout: 100},
                cursor: {batchSize: 0},
            }),
        );
        assert.neq(response.cursor.id, 0);

        stopServerReplication(rst.getSecondary());

        const getMoreResponse = assert.commandFailedWithCode(
            testDB.runCommand({getMore: response.cursor.id, collection: source.getName()}),
            ErrorCodes.WriteConcernTimeout,
        );

        // Test the same thing but using the shell helpers.
        let error = assert.throws(() =>
            source.aggregate([stageSpec], {cursor: {batchSize: 0}, writeConcern: {w: 2, wtimeout: 100}}).itcount(),
        );
        // Unfortunately this is the best way we have to check that the cause of the failure was due
        // to write concern. The aggregate shell helper will assert the command worked. When this
        // fails (as we expect due to write concern) it will create a new error object which loses
        // all structure and just preserves the information as text.
        assert(error instanceof Error);
        assert(error.writeConcernError, tojson(error));

        // Now test without batchSize just to be sure.
        error = assert.throws(() => source.aggregate([stageSpec], {writeConcern: {w: 2, wtimeout: 100}}));
        assert(error instanceof Error);
        assert(error.writeConcernError, tojson(error));

        restartServerReplication(rst.getSecondary());
        rst.awaitReplication();
    });
} finally {
    MongoRunner.runHangAnalyzer.enable();
}

rst.stopSet();
