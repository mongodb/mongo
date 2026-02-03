/**
 * Tests that a primary shard failover during unshardCollection commit/cleanup stage properly
 * handles DDL locks and doesn't cause premature lock release or server crashes.
 *
 * This test reproduces the scenario where:
 * 1. Resharding (unshardCollection) starts and proceeds normally
 * 2. Coordinator persists commit decision (sets unsplittable: true)
 * 3. Before coordinator tells participants to commit, primary shard steps down
 * 4. After step-up, DDL coordinator is rebuilt and should properly rejoin the operation
 * 5. DDL locks should be held during the rebuild to prevent concurrent DDL operations
 * 6. Operation completes successfully with proper cleanup
 *
 * @tags: [
 *   featureFlagUnshardCollection,
 *   config_shard_incompatible,
 * ]
 */

import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const reshardingTest = new ReshardingTest({
    numDonors: 1,
    numRecipients: 1,
    reshardInPlace: true,
    enableElections: true, // Enable replica sets with elections so stepdown works
});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const dbName = jsTestName();
const collName = "coll";
const sourceCollection = reshardingTest.createShardedCollection({
    ns: dbName + "." + collName,
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

const numDocs = 100;
const bulk = sourceCollection.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({oldKey: i, newKey: numDocs - i});
}
assert.commandWorked(bulk.execute());

const hangFailpoints = [];
reshardingTest._st.forEachConfigServer((configServer) => {
    hangFailpoints.push(configureFailPoint(configServer, "reshardingPauseBeforeTellingParticipantsToCommit"));
});

reshardingTest.withUnshardCollectionInBackground({toShard: donorShardNames[0]}, () => {}, {
    postDecisionPersistedFn: () => {
        jsTest.log("Decision persisted, waiting for failpoint");

        const configPrimary = reshardingTest._st.configRS.getPrimary();
        const hangFp = hangFailpoints.find((fp) => fp.conn.host === configPrimary.host);
        hangFp.wait();
        jsTest.log("Resharding coordinator paused, triggering stepdown on primary shard");

        const donorShard = reshardingTest.getReplSetForShard(donorShardNames[0]);
        assert.commandWorked(donorShard.getPrimary().adminCommand({replSetStepDown: 10, force: true}));
        donorShard.awaitNodesAgreeOnPrimary();

        // Set a short DDL lock timeout (500ms) to quickly verify locks are held during
        // failover.
        let ddlLockTimeoutFp = configureFailPoint(donorShard.getPrimary(), "overrideDDLLockTimeout", {
            "timeoutMillisecs": 500,
        });

        const dropResult = sourceCollection.getDB().runCommand({drop: sourceCollection.getName()});
        assert.commandFailedWithCode(
            dropResult,
            ErrorCodes.LockBusy,
            "DDL locks should prevent drop while resharding coordinator is in progress.",
        );
        jsTest.log("Confirmed: DDL locks are properly held during failover");

        ddlLockTimeoutFp.off();
        hangFailpoints.forEach((fp) => fp.off());
    },
    afterReshardingFn: () => {
        jsTest.log("unshardCollection completed successfully after failover");
        const collEntry = reshardingTest._st.config.collections.findOne({
            _id: sourceCollection.getFullName(),
        });
        assert.eq(true, collEntry.unsplittable, "Collection should be marked as unsplittable");
    },
});

reshardingTest.teardown();
