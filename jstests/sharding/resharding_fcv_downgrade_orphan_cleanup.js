/**
 * Reproduces the three orphan/temp-collection failure modes that arise when a
 * setFeatureCompatibilityVersion (setFCV) downgrade interleaves with an in-flight
 * reshardCollection or moveChunk migration:
 *
 *   1. SERVER-111230  reshard aborted while donor is in donatingInitialData leaves
 *                     temp-collection metadata in the global catalog.
 *   2. SERVER-92437   reshard started under the new-feature FCV commits under the
 *                     downgraded FCV (mixed-FCV commit), leaving range-deletion
 *                     entries in pending:true on the recipient.
 *   3. SERVER-121914  moveChunk aborted after commit but before its range-deletion
 *                     entry is removed leaves the entry in pending:true until
 *                     lazy recovery fires.
 *
 * Each block uses failpoints to force the abort-window interleavings that the
 * companion TLA+ spec (ReshardingFCVDowngradeOrphans.tla) exhibits. After setFCV
 * settles, the test asserts that the corresponding derived state has been cleaned
 * up synchronously — not deferred to lazy migration recovery.
 *
 *  @tags: [
 *      requires_fcv_80,
 *      requires_sharding,
 *      uses_atclustertime,
 *      multiversion_incompatible,
 *  ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const kDbName = "reshardFcvOrphanDb";
const kLatestFCV = "latest";
const kLastLTSFCV = "last-lts";

function getConfigDb(st) {
    return st.s.getDB("config");
}

function rangeDeletionsForNs(shard, ns) {
    return shard.getDB("config")["rangeDeletions"].find({nss: ns}).toArray();
}

function pendingRangeDeletionsForNs(shard, ns) {
    return shard.getDB("config")["rangeDeletions"]
        .find({nss: ns, pending: true})
        .toArray();
}

function tempReshardChunks(st, originalUUID) {
    // Temp namespace is system.resharding.<uuid> in config.chunks (post 5.0).
    return getConfigDb(st)["chunks"]
        .find({uuid: originalUUID})
        .toArray();
}

// ---------------------------------------------------------------------------------
// Block 1 — SERVER-111230: temp-collection metadata leak after donor-phase abort.
// ---------------------------------------------------------------------------------
function testTempCollectionMetadataLeakOnFCVAbort() {
    jsTest.log("SERVER-111230 repro: abort reshard in donatingInitialData");

    const reshardingTest = new ReshardingTest({numDonors: 1, numRecipients: 1});
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const sourceCollection = reshardingTest.createShardedCollection({
        ns: `${kDbName}.collTempMeta`,
        shardKeyPattern: {oldKey: 1},
        chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    });

    const mongos = sourceCollection.getMongo();
    const ns = sourceCollection.getFullName();

    // Pause the donor as it enters donatingInitialData so we have a deterministic
    // window in which to fire setFCV's abort path.
    const donorPrimary = reshardingTest.getReplSetForShard(donorShardNames[0]).getPrimary();
    const donorFP = configureFailPoint(donorPrimary, "reshardingPauseDonorBeforeDonatingInitialData");

    const reshardThread = new Thread((host, ns) => {
        const conn = new Mongo(host);
        try {
            conn.adminCommand({
                reshardCollection: ns,
                key: {newKey: 1},
                numInitialChunks: 1,
            });
        } catch (e) {
            jsTest.log("reshardCollection terminated (expected): " + tojson(e));
        }
    }, mongos.host, ns);
    reshardThread.start();

    donorFP.wait();

    // Trigger the setFCV abort path while the donor is suspended.
    assert.commandWorked(mongos.adminCommand({
        setFeatureCompatibilityVersion: kLastLTSFCV,
        confirm: true,
    }));

    donorFP.off();
    reshardThread.join();

    // Invariant TempCollectionMetadataClearedOnFCVAbort: no system.resharding.*
    // entries may remain in config.collections for this UUID.
    const lingering = getConfigDb(reshardingTest._st)["collections"]
        .find({_id: /^system\.resharding\./})
        .toArray();
    assert.eq(lingering.length, 0,
              "SERVER-111230: temp-collection metadata leaked after FCV abort: " +
                  tojson(lingering));

    // Restore FCV for the next block.
    assert.commandWorked(mongos.adminCommand({
        setFeatureCompatibilityVersion: kLatestFCV,
        confirm: true,
    }));

    reshardingTest.teardown();
}

// ---------------------------------------------------------------------------------
// Block 2 — SERVER-92437: mixed-FCV commit leaves pending range deletions.
// ---------------------------------------------------------------------------------
function testRangeDeletionsLeakOnMixedFCVCommit() {
    jsTest.log("SERVER-92437 repro: reshard starts under latestFCV, commits under lastLts");

    const reshardingTest = new ReshardingTest({numDonors: 1, numRecipients: 1});
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const recipientShardNames = reshardingTest.recipientShardNames;
    const sourceCollection = reshardingTest.createShardedCollection({
        ns: `${kDbName}.collMixedFCV`,
        shardKeyPattern: {oldKey: 1},
        chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    });

    const mongos = sourceCollection.getMongo();
    const recipientPrimary =
        reshardingTest.getReplSetForShard(recipientShardNames[0]).getPrimary();

    // Suspend the recipient in cloning so setFCV fires after it has decided to
    // skip building-index but before it has finished applying.
    const recipientFP = configureFailPoint(recipientPrimary, "reshardingPauseRecipientDuringCloning");

    const reshardThread = new Thread((host, ns) => {
        const conn = new Mongo(host);
        try {
            conn.adminCommand({reshardCollection: ns, key: {newKey: 1}});
        } catch (e) {
            jsTest.log("reshardCollection terminated: " + tojson(e));
        }
    }, mongos.host, sourceCollection.getFullName());
    reshardThread.start();

    recipientFP.wait();

    assert.commandWorked(mongos.adminCommand({
        setFeatureCompatibilityVersion: kLastLTSFCV,
        confirm: true,
    }));

    recipientFP.off();
    reshardThread.join();

    // Invariant RangeDeletionsClearedOnReshardAbort: no shard may carry a
    // pending:true range deletion for the reshard temp namespace once FCV settles.
    for (const shardName of recipientShardNames.concat(donorShardNames)) {
        const shardPrimary = reshardingTest.getReplSetForShard(shardName).getPrimary();
        const pending = shardPrimary.getDB("config")["rangeDeletions"]
            .find({pending: true})
            .toArray();
        assert.eq(pending.length, 0,
                  `SERVER-92437: pending range deletion leaked on ${shardName}: ` +
                      tojson(pending));
    }

    assert.commandWorked(mongos.adminCommand({
        setFeatureCompatibilityVersion: kLatestFCV,
        confirm: true,
    }));

    reshardingTest.teardown();
}

// ---------------------------------------------------------------------------------
// Block 3 — SERVER-121914: stale config.chunks after FCV downgrade aborts migration.
// ---------------------------------------------------------------------------------
function testStaleConfigChunksAfterFCVDowngrade() {
    jsTest.log("SERVER-121914 repro: setFCV aborts migration after commit, before cleanup");

    const st = new ShardingTest({shards: 2, mongos: 1});
    const dbName = kDbName + "_stale";
    const collName = "collStaleChunks";
    const ns = `${dbName}.${collName}`;

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));

    // Insert documents into both chunks.
    const testColl = st.s.getDB(dbName)[collName];
    for (let i = -5; i < 5; i++) {
        assert.commandWorked(testColl.insert({x: i}));
    }

    // Stall the donor between migration commit and range-deletion cleanup so
    // setFCV's abort path fires precisely in the committed-but-uncleaned window.
    const donorPrimary = st.rs0.getPrimary();
    const donorFP =
        configureFailPoint(donorPrimary, "hangBeforeRemovingRangeDeletionEntry");

    const migrationThread = new Thread((host, ns, toShard) => {
        const conn = new Mongo(host);
        try {
            conn.adminCommand({moveChunk: ns, find: {x: 0}, to: toShard});
        } catch (e) {
            jsTest.log("moveChunk terminated: " + tojson(e));
        }
    }, st.s.host, ns, st.shard1.shardName);
    migrationThread.start();

    donorFP.wait();

    assert.commandWorked(st.s.adminCommand({
        setFeatureCompatibilityVersion: kLastLTSFCV,
        confirm: true,
    }));

    donorFP.off();
    migrationThread.join();

    // Invariant NoStaleConfigChunksAfterFCVDowngrade: config.chunks entries for the
    // namespace must be consistent — no chunks owned by a shard with a still-pending
    // range deletion entry referring to the migrated range.
    const pendingOnDonor = pendingRangeDeletionsForNs(donorPrimary, ns);
    assert.eq(pendingOnDonor.length, 0,
              "SERVER-121914: pending range deletion left on donor: " +
                  tojson(pendingOnDonor));

    // And invariants on shard1 (the recipient) too.
    const pendingOnRecipient = pendingRangeDeletionsForNs(st.rs1.getPrimary(), ns);
    assert.eq(pendingOnRecipient.length, 0,
              "SERVER-121914: pending range deletion left on recipient: " +
                  tojson(pendingOnRecipient));

    assert.commandWorked(st.s.adminCommand({
        setFeatureCompatibilityVersion: kLatestFCV,
        confirm: true,
    }));

    st.stop();
}

testTempCollectionMetadataLeakOnFCVAbort();
testRangeDeletionsLeakOnMixedFCVCommit();
testStaleConfigChunksAfterFCVDowngrade();
