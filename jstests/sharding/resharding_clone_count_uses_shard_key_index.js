/**
 * Tests the per-donor "documents to copy" count during resharding verification.
 *
 * 1. With a shard-key-prefixed index: each donor counts only its own documents via that index.
 * 2. Without one (e.g. hashed index dropped): the coordinator skips clone verification instead of
 *    running an uncovered count. Resharding still completes successfully.
 *
 * @tags: [
 *   uses_atclustertime,
 *   featureFlagReshardingVerification,
 *   requires_fcv_90,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const mongos = reshardingTest._st.s;

// --- Scenario 1: ranged shard key, shard-filtered covered count, orphans excluded ---
{
    jsTestLog(
        "Scenario 1: ranged shard key — each donor reports a shard-filtered covered count, " +
            "with planted orphans excluded.",
    );

    const ns = "reshardingDb.coll";
    const sourceCollection = reshardingTest.createShardedCollection({
        ns,
        shardKeyPattern: {oldKey: 1},
        chunks: [
            {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
            {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
        ],
    });

    // Populate the collection with documents owned by each donor.
    const numOwnedByDonor0 = 5;
    const numOwnedByDonor1 = 8;
    const ownedDocs = [];
    for (let i = 1; i <= numOwnedByDonor0; i++) {
        ownedDocs.push({_id: `d0-${i}`, oldKey: -i, newKey: -i});
    }
    for (let i = 1; i <= numOwnedByDonor1; i++) {
        ownedDocs.push({_id: `d1-${i}`, oldKey: i, newKey: i});
    }
    assert.commandWorked(sourceCollection.insert(ownedDocs));

    // Insert orphan documents to prove they are excluded from the count.
    TestData.skipCheckOrphans = true;
    const donor0Primary = reshardingTest.getReplSetForShard(donorShardNames[0]).getPrimary();
    const donor1Primary = reshardingTest.getReplSetForShard(donorShardNames[1]).getPrimary();
    function insertOrphan(donorPrimary, doc) {
        assert.commandWorked(
            donorPrimary.getCollection(ns).runCommand("insert", {
                documents: [doc],
                shardVersion: ShardVersioningUtil.kIgnoredShardVersion,
            }),
        );
    }
    insertOrphan(donor0Primary, {_id: "orphan-on-d0", oldKey: 1000, newKey: 1000});
    insertOrphan(donor1Primary, {_id: "orphan-on-d1", oldKey: -1000, newKey: -1000});

    const pauseBeforeApplyingFp = configureFailPoint(
        reshardingTest._st.configRS.getPrimary(),
        "reshardingPauseCoordinatorBeforeApplying",
    );

    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
                {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
            ],
            performVerification: true,
        },
        () => {
            pauseBeforeApplyingFp.wait();

            const coordinatorDoc = mongos
                .getCollection("config.reshardingOperations")
                .findOne({ns});
            assert.neq(coordinatorDoc, null, "expected coordinator document to exist");

            const documentsToCopy = {};
            for (const donor of coordinatorDoc.donorShards) {
                documentsToCopy[donor.id] = donor.documentsToCopy;
            }

            jsTest.log.info("Fetched per-donor documentsToCopy", {documentsToCopy});

            // The counts exclude the planted orphans: each donor reports only what it owns.
            assert.eq(documentsToCopy[donorShardNames[0]], numOwnedByDonor0, {documentsToCopy});
            assert.eq(documentsToCopy[donorShardNames[1]], numOwnedByDonor1, {documentsToCopy});

            // The count has been persisted (it was a snapshot read at the clone timestamp), so
            // remove the orphans now. Otherwise the fixture's end-of-operation consistency check,
            // which reads the source with orphan-inclusive "available" read concern, would flag
            // them as missing after resharding.
            assert.commandWorked(donor0Primary.getCollection(ns).remove({_id: "orphan-on-d0"}));
            assert.commandWorked(donor1Primary.getCollection(ns).remove({_id: "orphan-on-d1"}));

            pauseBeforeApplyingFp.off();
        },
    );
}

// --- Scenario 2: hashed shard key with index dropped, clone verification skipped ---
{
    jsTestLog(
        "Scenario 2: hashed shard key with its index dropped — no index can cover the count, " +
            "so the coordinator skips clone verification and resharding still completes.",
    );

    const hashedNs = "reshardingDb.collHashed";
    const hashedSourceCollection = reshardingTest.createShardedCollection({
        ns: hashedNs,
        shardKeyPattern: {oldKey: "hashed"},
        chunks: [
            {min: {oldKey: MinKey}, max: {oldKey: NumberLong(0)}, shard: donorShardNames[0]},
            {min: {oldKey: NumberLong(0)}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
        ],
    });

    assert.commandWorked(
        hashedSourceCollection.insert(
            Array.from({length: 20}, (_, i) => ({_id: i, oldKey: i, newKey: i})),
        ),
    );

    // Dropping the hashed shard-key index leaves the donors with no index that can cover the count.
    assert.commandWorked(hashedSourceCollection.dropIndex({oldKey: "hashed"}));

    const pauseBeforeApplyingFp = configureFailPoint(
        reshardingTest._st.configRS.getPrimary(),
        "reshardingPauseCoordinatorBeforeApplying",
    );

    // Resharding must still commit even though the clone count is skipped.
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
                {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
            ],
            performVerification: true,
        },
        () => {
            pauseBeforeApplyingFp.wait();

            const coordinatorDoc = mongos
                .getCollection("config.reshardingOperations")
                .findOne({ns: hashedNs});
            assert.neq(coordinatorDoc, null, "expected coordinator document to exist");

            for (const donor of coordinatorDoc.donorShards) {
                assert.eq(
                    donor.documentsToCopy,
                    undefined,
                    "expected clone count to be skipped for donor",
                    {donor},
                );
            }

            pauseBeforeApplyingFp.off();
        },
    );
}

reshardingTest.teardown();
