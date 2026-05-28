/**
 * Tests that the resharding operation will fail if a recipient shard would have missed oplog
 * entries from a donor shard.
 * @tags: [
 * ]
 */
import {runOplogSyncAggAssertMinOplogTest} from "jstests/sharding/libs/resharding_oplog_helpers.js";

runOplogSyncAggAssertMinOplogTest({
    setupCollection(testDB, testColl) {
        // Plain collection: no explicit creation needed.
    },
    findAnchorOplogEntry(localDb, testColl, lastBeforeTs) {
        return localDb.oplog.rs.findOne({"op": "i", "o._id": 0});
    },
    insertNextDoc(testColl, id, longString) {
        return testColl.insert({_id: id, longString: longString});
    },
});
