/**
 * Verifies that a change stream opened with 'showEndOfTransaction: "required"' fails on a
 * subsequent getMore if the end-of-transaction feature is disabled after the stream was opened, so
 * the consumer learns that the stream can no longer emit endOfTransaction events.
 *
 * The end-of-transaction feature requires the 'featureFlagEndOfTransactionChangeEventSynthesis'
 * feature flag, which is startup-only. Tests only run in suites that enable the flag at startup
 * (e.g. the 'change_streams_eot_*' passthroughs); elsewhere the test skips.
 *
 * @tags: [
 *   requires_fcv_90,
 *   uses_change_streams,
 *   featureFlagChangeStreamReaderV2,
 *   # The kill switch only applies to replica-set and router (mongos) streams. A shard-local
 *   # stream (a direct-to-mongod or '$_passthroughToShard' stream) treats 'required' as
 *   # 'whenAvailable' and never fails, so this test cannot run when traffic goes straight to a
 *   # shard. See isShardLocalStream() in change_stream_helpers.cpp.
 *   directly_against_shardsvrs_incompatible,
 * ]
 */

import {after, before, describe} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {itWhenEndOfTransactionIsUsable} from "jstests/libs/query/change_stream_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

describe("showEndOfTransaction: required fails when the feature is disabled after open", function () {
    const collName = jsTestName();

    before(function () {
        assertDropAndRecreateCollection(db, collName);
    });

    after(function () {
        assertDropCollection(db, collName);
    });

    itWhenEndOfTransactionIsUsable(
        db,
        "fails a getMore after 'allowEndOfTransaction' is turned off",
        function () {
            assert.commandWorked(db[collName].insertOne({_id: 1}));

            const openRes = assert.commandWorked(
                db.runCommand({
                    aggregate: collName,
                    pipeline: [{$changeStream: {showEndOfTransaction: "required"}}],
                    cursor: {},
                }),
            );
            const cursorId = openRes.cursor.id;

            // While the feature is enabled, getMore succeeds.
            assert.commandWorked(db.runCommand({getMore: cursorId, collection: collName}));

            // Disable the feature on all nodes after the stream was opened. The next getMore must
            // fail with a descriptive, non-resumable error. The helper restores the original
            // parameter value on all nodes when it returns.
            runWithParamsAllNonConfigNodes(db, {allowEndOfTransaction: false}, () => {
                assert.commandFailedWithCode(
                    db.runCommand({getMore: cursorId, collection: collName}),
                    ErrorCodes.ChangeStreamFatalError,
                );
            });
        },
    );
});
