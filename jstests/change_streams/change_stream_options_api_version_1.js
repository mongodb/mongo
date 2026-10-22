/**
 * Tests that change stream options that are not part of API Version 1 are rejected when a change
 * stream is opened with 'apiVersion: "1"' and 'apiStrict: true'.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {isEndOfTransactionUsable} from "jstests/libs/query/change_stream_util.js";

const testDB = db.getSiblingDB(jsTestName());

function assertRejectedWithApiStrict(changeStreamSpec) {
    assert.commandFailedWithCode(
        testDB.runCommand({
            aggregate: 1,
            pipeline: [{$changeStream: changeStreamSpec}],
            cursor: {},
            apiVersion: "1",
            apiStrict: true,
        }),
        ErrorCodes.APIStrictError,
    );
}

// None of these options are part of API Version 1.
assertRejectedWithApiStrict({showExpandedEvents: true});
assertRejectedWithApiStrict({showRawUpdateDescription: true});
assertRejectedWithApiStrict({showSystemEvents: true});

// 'showEndOfTransaction' is likewise not part of API Version 1, but it can only be used if the
// end-of-transaction feature is enabled.
if (isEndOfTransactionUsable(db)) {
    assertRejectedWithApiStrict({showEndOfTransaction: "off"});
    assertRejectedWithApiStrict({showEndOfTransaction: "whenAvailable"});
    assertRejectedWithApiStrict({showEndOfTransaction: "required"});
}
