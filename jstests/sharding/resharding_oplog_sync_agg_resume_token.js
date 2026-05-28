/**
 * Test that the postBatchResumeToken field is only included for the oplog namespace when
 * $_requestReshardingResumeToken is specified for an aggregate command.
 *
 * @tags: [
 * ]
 */
import {runOplogSyncAggResumeTokenTest} from "jstests/sharding/libs/resharding_oplog_helpers.js";

runOplogSyncAggResumeTokenTest({
    setupCollection(testDB, collName) {
        // Plain collection: no explicit creation needed.
    },
    makeDocument(i) {
        return {x: i};
    },
    oplogFilterField: "o.x",
});
