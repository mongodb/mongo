/**
 * Regression test for SERVER-127745: when a resharding recipient node goes through a failover
 * during the building-index state, a new ReshardingRecipientService instance is created and waits
 * for the pre-existing index build to finish. That wait must be interruptible by stepdown.
 *
 * The ReshardingRecipientService instance waiting for the pre-existing index build to finish used
 * to be uninterruptible and ignored every stepdown interrupt, so an election won by the same node
 * while the instance was waiting for the index build to finish deadlocked permanently:
 * PrimaryOnlyService::onStepUp() joined the blocked previous-term instance while holding the RSTL
 * in X mode, and the index build could neither commit nor abort until the node became writable.
 * The node reported PRIMARY but never became a writable primary.
 *
 * The test recreates that scenario with two step-down/step-up flaps of the resharding recipient
 * primary:
 * Flap #1 gets the rebuilt ReshardingRecipientService instance waiting on the pre-existing index
 * build.
 * Flap #2 delivers a stepdown interrupt while the instance is waiting on the index build. The index
 * build is kept alive throughout by starving commit quorum.
 * The test then asserts the node becomes a writable primary again, i.e. the interrupt
 * landed, the ReshardingRecipientService instance unwound, and the step-up completed.
 *
 * @tags: [
 *   requires_persistence,
 *   resource_intensive,
 *   uses_atclustertime,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

describe("resharding recipient waiting on a pre-existing index build", function () {
    const ns = "reshardingDb.test_coll";

    let reshardingTest;
    let recipientShardName;
    let recipientReplSet;
    let recipientNodeHost;
    let secondaries;
    let secondaryBuildFps;

    // During the deadlock window the primary-elect reports isWritablePrimary=false, so
    // recipientReplSet.getPrimary() would hang; always talk to the node directly.
    function nodeConn() {
        return new Mongo(recipientNodeHost);
    }

    function isWritable() {
        try {
            return nodeConn().adminCommand({hello: 1}).isWritablePrimary === true;
        } catch (e) {
            return false;
        }
    }

    function rapidStepDownStepUpSameNode() {
        assert.commandWorked(nodeConn().adminCommand({replSetStepDown: 60, force: true}));
        assert.soonNoExcept(() => nodeConn().adminCommand({replSetFreeze: 0}).ok);
        assert.soonNoExcept(() => nodeConn().adminCommand({replSetStepUp: 1}).ok);

        // Wait for the node to hold the PRIMARY role.
        assert.soonNoExcept(
            () => nodeConn().adminCommand({replSetGetStatus: 1}).myState === 1,
            "node did not win the election after replSetStepUp",
        );
        // Wait for the node to become a writable primary.
        // The deadlock would prevent the node from becoming writable during the second flap.
        assert.soon(isWritable, "node did not become a writable primary again");
    }

    function isReshardingRecipientOpInBuildingIndexState(conn) {
        const doc = conn.getCollection("config.localReshardingOperations.recipient").findOne();
        return doc?.mutableState?.state === "building-index";
    }

    function inProgressBuildUUIDs(conn, tempNs) {
        const dotIdx = tempNs.indexOf(".");
        const dbName = tempNs.substring(0, dotIdx);
        const collName = tempNs.substring(dotIdx + 1);
        const res = conn.getDB(dbName).runCommand({listIndexes: collName, includeBuildUUIDs: true});
        if (!res.ok) {
            return [];
        }
        const uuids = res.cursor.firstBatch
            .filter((e) => e.buildUUID !== undefined)
            .map((e) => e.buildUUID.toString());
        return [...new Set(uuids)];
    }

    function getReshardingRecipientServiceOps(conn) {
        return conn
            .getDB("admin")
            .aggregate([
                {$currentOp: {allUsers: true, idleConnections: true}},
                {$match: {desc: {$regex: "^ReshardingRecipientService"}}},
            ])
            .toArray();
    }

    // Unblocking the secondaries' index builds is best-effort: an error here must not mask the
    // error that triggered the cleanup, and one unreachable secondary must not prevent the other
    // from being released.
    function releaseSecondaryBuildFps() {
        if (!secondaryBuildFps) {
            return;
        }
        secondaryBuildFps.forEach((fp) => {
            try {
                fp.off();
            } catch (e) {
                jsTest.log.info("Ignoring error while releasing an index build failpoint", {
                    error: e.toString(),
                });
            }
        });
        secondaryBuildFps = [];
    }

    before(function () {
        reshardingTest = new ReshardingTest({
            numDonors: 1,
            numRecipients: 1,
            enableElections: true,
            minimumOperationDurationMS: 0,
        });
        reshardingTest.setup();

        const donorShardName = reshardingTest.donorShardNames[0];
        recipientShardName = reshardingTest.recipientShardNames[0];

        const sourceCollection = reshardingTest.createShardedCollection({
            ns: ns,
            shardKeyPattern: {oldKey: 1},
            chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardName}],
        });

        // Extra indexes so the building-index state has real work.
        assert.commandWorked(sourceCollection.createIndex({extraA: 1}));
        assert.commandWorked(sourceCollection.createIndex({extraB: -1}));

        assert.commandWorked(
            sourceCollection.insert(
                Array.from({length: 200}, (_, i) => ({
                    _id: i,
                    oldKey: i % 2 === 0 ? -i - 1 : i + 1,
                    newKey: i % 2 === 0 ? i + 1 : -i - 1,
                    extraA: i,
                    extraB: -i,
                })),
            ),
        );

        recipientReplSet = reshardingTest.getReplSetForShard(recipientShardName);
        recipientNodeHost = recipientReplSet.getPrimary().host;
        secondaries = recipientReplSet.getSecondaries();

        // Frozen secondaries still vote and replicate but cannot run for election, so the recipient
        // primary cannot move under us.
        secondaries.forEach((s) =>
            assert.commandWorked(s.adminCommand({replSetFreeze: ReplSetTest.kForeverSecs})),
        );

        // Prevent commit quorum for index build: the secondaries block their build right after
        // registration and never vote commit readiness, so the primary-side build stays registered
        // indefinitely.
        secondaryBuildFps = secondaries.map((s) =>
            configureFailPoint(s, "hangAfterInitializingIndexBuild"),
        );
    });

    after(function () {
        releaseSecondaryBuildFps();
        reshardingTest.teardown();
    });

    it("becomes a writable primary again after a rapid same-node stepdown/step-up", function () {
        reshardingTest.withReshardingInBackground(
            {
                newShardKeyPattern: {newKey: 1},
                newChunks: [
                    {min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardName},
                ],
            },
            (tempNs) => {
                try {
                    runTest(tempNs);
                } finally {
                    releaseSecondaryBuildFps();
                }
            },
        );
    });

    function runTest(tempNs) {
        // Wait until the recipient is in building-index with a live build on the primary.
        assert.soonNoExcept(
            () => isReshardingRecipientOpInBuildingIndexState(nodeConn()),
            "recipient never reached the building-index state",
            5 * 60 * 1000,
        );

        let origBuildUUIDs;
        assert.soonNoExcept(
            () => {
                origBuildUUIDs = inProgressBuildUUIDs(nodeConn(), tempNs);
                return origBuildUUIDs.length >= 1;
            },
            "no index build ever registered on the recipient primary",
            5 * 60 * 1000,
        );

        // Both secondaries are blocked on their index build.
        secondaryBuildFps.forEach((fp) => fp.wait());

        // Flap #1 makes the ReshardingRecipientService instance wait on the pre-existing index
        // build: the failover interrupts the original instance, and the instance rebuilt on step-up
        // finds the surviving build and waits for it to finish.
        jsTest.log.info(
            "Flap #1: fail over so the rebuilt ReshardingRecipientService instance waits on" +
                " the pre-existing index build",
            {recipientNodeHost},
        );
        rapidStepDownStepUpSameNode();

        // The deadlock condition are met when the rebuilt ReshardingRecipientService instance's
        // opCtx is alive and stable, the state doc is still building-index, and no second build was
        // registered: proving the instance is waiting for the pre-existing index build to finish.
        let stable = null;
        assert.soonNoExcept(
            () => {
                const conn = nodeConn();
                if (!isReshardingRecipientOpInBuildingIndexState(conn)) {
                    stable = null;
                    return false;
                }
                const buildUUIDs = inProgressBuildUUIDs(conn, tempNs);
                if (buildUUIDs.length !== 1 || buildUUIDs[0] !== origBuildUUIDs[0]) {
                    stable = null;
                    return false;
                }
                const ops = getReshardingRecipientServiceOps(conn);
                if (ops.length === 0) {
                    stable = null;
                    return false;
                }
                const key = ops
                    .map((op) => op.opid)
                    .sort()
                    .join(",");
                if (stable !== null && stable.key === key && Date.now() - stable.since >= 3000) {
                    return true;
                }
                if (stable === null || stable.key !== key) {
                    stable = {key: key, since: Date.now()};
                }
                return false;
            },
            "the rebuilt recipient instance never blocked awaiting the index build",
            5 * 60 * 1000,
            500,
        );

        // Flap #2 delivers a stepdown interrupt to the ReshardingRecipientService instance while it
        // waits on the index build.
        // Before the fix, the interrupt was ignored and the subsequent step-up deadlocked joining
        // the still-waiting instance.
        jsTest.log.info(
            "Flap #2: fail over while the ReshardingRecipientService instance waits on the index build",
            {recipientNodeHost},
        );
        rapidStepDownStepUpSameNode();
    }
});
