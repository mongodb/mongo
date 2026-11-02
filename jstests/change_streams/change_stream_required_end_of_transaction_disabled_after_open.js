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
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {isEndOfTransactionUsable} from "jstests/libs/query/change_stream_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

describe("showEndOfTransaction: required fails when the feature is disabled after open", function () {
    const collName = jsTestName();

    before(function () {
        assertDropAndRecreateCollection(db, collName);
    });

    after(function () {
        assertDropCollection(db, collName);
    });

    it("fails a getMore after 'allowEndOfTransaction' is turned off", function () {
        if (!isEndOfTransactionUsable(db)) {
            jsTest.log.info("Skipping test: end-of-transaction change streams are not usable");
            return;
        }

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

        // Disable the feature on all nodes after the stream was opened. The next getMore must fail
        // with a descriptive, non-resumable error. The helper restores the original parameter value
        // on all nodes when it returns.
        runWithParamsAllNonConfigNodes(db, {allowEndOfTransaction: false}, () => {
            assert.commandFailedWithCode(
                db.runCommand({getMore: cursorId, collection: collName}),
                ErrorCodes.ChangeStreamFatalError,
            );
        });
    });
});
