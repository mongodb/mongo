/**
 * Enables a failpoint that recreates an interruption on the OpCtx while planShardedSearch is
 * executing, and asserts that the correct error is thrown instead of the server segfaulting.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_unsharded_collection,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

// The failpoint must be enabled on the same router that executes the aggregate.
TestData.pinToSingleMongos = true;

const testDb = db.getSiblingDB(jsTestName());
const collName = jsTestName();
const testColl = testDb.getCollection(collName);

describe("sharded $search network interruption", function () {
    before(function () {
        testColl.drop();
        assert.commandWorked(testColl.insertMany([{_id: 1}, {_id: 20}]));

        // The collection must be sharded so that the aggregate goes through planShardedSearch.
        assert.commandWorked(
            testDb.adminCommand({shardCollection: testColl.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(
            testDb.adminCommand({split: testColl.getFullName(), middle: {_id: 10}}),
        );

        assert.commandWorked(
            testDb.adminCommand({
                configureFailPoint: "shardedSearchOpCtxDisconnect",
                mode: "alwaysOn",
            }),
        );
    });

    after(function () {
        assert.commandWorked(
            testDb.adminCommand({configureFailPoint: "shardedSearchOpCtxDisconnect", mode: "off"}),
        );
        testColl.drop();
    });

    it("should return an interruption error rather than crash", function () {
        const error = assert.throws(() => testColl.aggregate([{$search: {}}]));
        assert.commandFailedWithCode(error, ErrorCodes.Interrupted);

        // Make sure the router is still up.
        assert.commandWorked(testDb.runCommand("ping"));
    });
});
