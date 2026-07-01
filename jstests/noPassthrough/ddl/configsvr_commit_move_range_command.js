/**
 * Tests the internal `_configsvrCommitMoveRange` command. The command commits a chunk migration to
 * the global catalog and returns the list of chunks it changed.
 *
 * It is always issued as a retryable write: it requires a session id, runs the commit under an
 * alternative client, and then makes a dummy write so an older request on the same session cannot
 * replay the operation onto a newer state. Each attempt advances the session's txnNumber;
 * idempotency comes from the catalog-level "already migrated" check, not from retryable-write dedup.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, afterEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Builds the BSON serialization of a ChunkVersion: the collection generation (epoch + timestamp)
// plus the placement version (the chunk's `lastmod` Timestamp(major, minor)).
function chunkVersion(epoch, timestamp, lastmod) {
    return {e: epoch, t: timestamp, v: lastmod};
}

// Asserts two changed-chunks lists describe the same chunks, independent of order. Only the chunk
// identity (range, shard, version) is compared: the normal commit path and the idempotent rebuild
// path can differ in incidental fields.
function assertSameChangedChunks(lhs, rhs) {
    const sameChunk = (a, b) =>
        bsonWoCompare(a.min, b.min) === 0 &&
        bsonWoCompare(a.max, b.max) === 0 &&
        a.shard === b.shard &&
        bsonWoCompare(a.lastmod, b.lastmod) === 0;
    assert.sameMembers(lhs, rhs, "changed chunks differ", sameChunk, {lhs, rhs});
}

// Finds the chunk in 'changed' covering exactly [min, max).
function findChunk(changed, min, max) {
    return changed.find((c) => bsonWoCompare(c.min, min) === 0 && bsonWoCompare(c.max, max) === 0);
}

describe("_configsvrCommitMoveRange", function () {
    const dbName = "commit_move_range_db";

    before(function () {
        this.st = new ShardingTest({shards: 2});
        this.config = this.st.configRS.getPrimary().getDB("config");
        this.shard0 = this.st.shard0.shardName;
        this.shard1 = this.st.shard1.shardName;

        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: dbName, primaryShard: this.shard0}),
        );

        this.getChunk = (skValue) =>
            this.st.s
                .getCollection("config.chunks")
                .findOne({uuid: this.collUuid, min: {$lte: skValue}, max: {$gt: skValue}});

        // Highest chunk version owned by 'shard', i.e. its shard placement version.
        this.getShardVersion = (shard) => {
            const top = this.st.s
                .getCollection("config.chunks")
                .find({uuid: this.collUuid, shard})
                .sort({lastmod: -1})
                .limit(1)
                .toArray();
            return top.length ? top[0].lastmod : null;
        };

        // Highest chunk version of the collection, i.e. its collection placement version.
        this.getCollectionVersion = () =>
            this.st.s
                .getCollection("config.chunks")
                .find({uuid: this.collUuid})
                .sort({lastmod: -1})
                .limit(1)
                .toArray()[0].lastmod;

        // Builds the (sessionless) global catalog commit command for moving the range
        // [movedMin, movedMax). Reads the current donor versions, so it must be called before the
        // migration is committed; the same cmd is then reused across retries.
        this.buildMoveRangeCmdSessionless = (movedMin, movedMax) => ({
            _configsvrCommitMoveRange: this.ns,
            fromShard: this.shard0,
            toShard: this.shard1,
            migratedChunk: {
                min: movedMin,
                max: movedMax,
                lastmod: chunkVersion(
                    this.collEpoch,
                    this.collTimestamp,
                    this.getChunk(movedMin).lastmod,
                ),
            },
            donorShardVersionPreMigration: chunkVersion(
                this.collEpoch,
                this.collTimestamp,
                this.getShardVersion(this.shard0),
            ),
            writeConcern: {w: "majority"},
        });

        // Attaches the session and the next txnNumber; every attempt advances the txnNumber.
        this.withSession = (cmd) => ({
            ...cmd,
            lsid: this.lsid,
            txnNumber: NumberLong(++this.txnCounter),
        });

        // Asserts that every changed chunk other than the expected ones (the migrated chunk and any
        // side chunks created by a split) is the donor control chunk: exactly one, on the donor. Its
        // identity is left unpinned because the catalog picks an arbitrary remaining donor chunk to
        // carry the bumped shard version.
        this.assertControlOnDonor = (changed, expected) => {
            const control = changed.filter((c) => !expected.includes(c));
            assert.eq(1, control.length, "expected exactly one control chunk", {changed});
            assert.eq(this.shard0, control[0].shard, "control chunk must be on donor", {control});
        };

        this.setAllowChunkOperations = (allow) => {
            assert.commandWorked(
                this.config.adminCommand({
                    _configsvrSetAllowChunkOperations: this.ns,
                    allowChunkOperations: allow,
                    writeConcern: {w: "majority"},
                }),
            );
        };
    });

    after(function () {
        this.st.stop();
    });

    beforeEach(function () {
        // Per-test collection so chunk state and the session are isolated.
        this.collName = "coll_" + new ObjectId().str;
        this.ns = dbName + "." + this.collName;

        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));

        // Build a diverse layout spread across both shards:
        //   shard0 (donor):     [-100, -50), [0, MaxKey)
        //   shard1 (recipient): [MinKey, -100), [-50, 0)
        // The donor keeps the large [0, MaxKey) chunk used by the split cases plus a separate
        // control chunk ([-100, -50)); the recipient already owns a couple of non-adjacent chunks.
        for (const middle of [{x: -100}, {x: -50}, {x: 0}]) {
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle}));
        }
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: -150}, to: this.shard1}),
        );
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: -25}, to: this.shard1}),
        );

        const coll = this.st.s.getCollection("config.collections").findOne({_id: this.ns});
        this.collEpoch = coll.lastmodEpoch;
        this.collTimestamp = coll.timestamp;
        this.collUuid = coll.uuid;
        this.preCollVersion = this.getCollectionVersion();

        this.lsid = assert.commandWorked(this.st.s.getDB("admin").runCommand({startSession: 1})).id;
        this.txnCounter = 0;
    });

    afterEach(function () {
        assert.commandWorked(this.st.s.getDB(dbName).runCommand({drop: this.collName}));
    });

    it("rejects a call without a session id", function () {
        assert.commandFailedWithCode(
            this.config.adminCommand(this.buildMoveRangeCmdSessionless({x: -100}, {x: -50})),
            ErrorCodes.IllegalOperation,
        );
    });

    it("fences a stale txnNumber on the same session", function () {
        const cmd = this.buildMoveRangeCmdSessionless({x: -100}, {x: -50});
        assert.commandWorked(this.config.adminCommand(this.withSession(cmd)));

        // A request that arrives with an older txnNumber than one already committed is rejected.
        assert.commandFailedWithCode(
            this.config.adminCommand({...cmd, lsid: this.lsid, txnNumber: NumberLong(0)}),
            ErrorCodes.TransactionTooOld,
        );
    });

    it("returns the migrated and control chunks", function () {
        // Move the whole [-100, -50) chunk; the donor keeps a separate control chunk.
        const cmd = this.buildMoveRangeCmdSessionless({x: -100}, {x: -50});
        const changed = assert.commandWorked(
            this.config.adminCommand(this.withSession(cmd)),
        ).changedChunks;

        assert.eq(2, changed.length, "expected migrated + control chunk", {changed});

        const migrated = findChunk(changed, {x: -100}, {x: -50});
        assert(migrated, "migrated chunk missing", {changed});
        assert.eq(this.shard1, migrated.shard, "migrated chunk not on recipient", {migrated});
        this.assertControlOnDonor(changed, [migrated]);
    });

    it("returns three chunks when the moved range splits the source chunk in two", function () {
        // Move [0, 10), a sub-range of [0, MaxKey) touching its left bound. The source chunk splits
        // into the moved chunk and one right side chunk; a separate control chunk is also returned.
        const cmd = this.buildMoveRangeCmdSessionless({x: 0}, {x: 10});
        const changed = assert.commandWorked(
            this.config.adminCommand(this.withSession(cmd)),
        ).changedChunks;

        assert.eq(3, changed.length, "expected migrated + right side + control chunk", {changed});

        const migrated = findChunk(changed, {x: 0}, {x: 10});
        const rightSide = findChunk(changed, {x: 10}, {x: MaxKey});
        assert(migrated, "migrated chunk missing", {changed});
        assert(rightSide, "right side chunk missing", {changed});
        assert.eq(this.shard1, migrated.shard, "migrated chunk not on recipient", {migrated});
        assert.eq(this.shard0, rightSide.shard, "right side chunk not on donor", {rightSide});
        this.assertControlOnDonor(changed, [migrated, rightSide]);
    });

    it("returns four chunks when the moved range splits the source chunk in three", function () {
        // Move [10, 20), a middle sub-range of [0, MaxKey). The source chunk splits into a left side,
        // the moved chunk, and a right side; a separate control chunk is also returned.
        const cmd = this.buildMoveRangeCmdSessionless({x: 10}, {x: 20});
        const changed = assert.commandWorked(
            this.config.adminCommand(this.withSession(cmd)),
        ).changedChunks;

        assert.eq(4, changed.length, "expected migrated + two side + control chunks", {changed});

        const migrated = findChunk(changed, {x: 10}, {x: 20});
        const leftSide = findChunk(changed, {x: 0}, {x: 10});
        const rightSide = findChunk(changed, {x: 20}, {x: MaxKey});
        assert(migrated, "migrated chunk missing", {changed});
        assert(leftSide, "left side chunk missing", {changed});
        assert(rightSide, "right side chunk missing", {changed});
        assert.eq(this.shard1, migrated.shard, "migrated chunk not on recipient", {migrated});
        assert.eq(this.shard0, leftSide.shard, "left side chunk not on donor", {leftSide});
        assert.eq(this.shard0, rightSide.shard, "right side chunk not on donor", {rightSide});
        this.assertControlOnDonor(changed, [migrated, leftSide, rightSide]);
    });

    it("bumps the placement version of the config server", function () {
        const cmd = this.buildMoveRangeCmdSessionless({x: -100}, {x: -50});
        assert.commandWorked(this.config.adminCommand(this.withSession(cmd)));

        const migrated = this.getChunk({x: -100});
        assert.eq(this.shard1, migrated.shard, "migrated chunk not on recipient", {migrated});
        // The migrated chunk's major version is the pre-migration collection version major + 1.
        assert.eq(this.preCollVersion.t + 1, migrated.lastmod.t, "major version not bumped", {
            preCollVersion: this.preCollVersion,
            migrated,
        });
    });

    it("is idempotent when retried after a successful attempt", function () {
        const cmd = this.buildMoveRangeCmdSessionless({x: -100}, {x: -50});

        const first = assert.commandWorked(
            this.config.adminCommand(this.withSession(cmd)),
        ).changedChunks;
        const versionAfterFirst = this.getShardVersion(this.shard1);

        // The retry without a session id is still rejected.
        assert.commandFailedWithCode(this.config.adminCommand(cmd), ErrorCodes.IllegalOperation);

        // The command is retried with an advanced txnNumber; the commit is detected as already
        // applied and returns the same changed chunks.
        const second = assert.commandWorked(
            this.config.adminCommand(this.withSession(cmd)),
        ).changedChunks;

        assertSameChangedChunks(first, second);
        assert.eq(
            0,
            bsonWoCompare(versionAfterFirst, this.getShardVersion(this.shard1)),
            "retry must not bump any version",
        );
    });

    it("commits when retried after a failed attempt", function () {
        const cmd = this.buildMoveRangeCmdSessionless({x: -100}, {x: -50});

        const fp = configureFailPoint(this.st.configRS.getPrimary(), "migrationCommitVersionError");
        assert.commandFailedWithCode(
            this.config.adminCommand(this.withSession(cmd)),
            ErrorCodes.StaleEpoch,
        );
        fp.off();

        // The failed attempt committed nothing: the chunk is still on the donor.
        assert.eq(this.shard0, this.getChunk({x: -100}).shard, "chunk must not have moved");

        // The retry without a session id is still rejected.
        assert.commandFailedWithCode(this.config.adminCommand(cmd), ErrorCodes.IllegalOperation);

        const changed = assert.commandWorked(
            this.config.adminCommand(this.withSession(cmd)),
        ).changedChunks;
        assert(findChunk(changed, {x: -100}, {x: -50}), "migrated chunk missing", {changed});
        assert.eq(this.shard1, this.getChunk({x: -100}).shard, "chunk must have moved");
    });

    it("returns OK on a retry after success even when chunk operations are disallowed", function () {
        const cmd = this.buildMoveRangeCmdSessionless({x: -100}, {x: -50});

        const first = assert.commandWorked(
            this.config.adminCommand(this.withSession(cmd)),
        ).changedChunks;
        const versionAfterFirst = this.getShardVersion(this.shard1);

        // Simulate a concurrent DDL setting allowChunkOperations to false.
        this.setAllowChunkOperations(false);
        try {
            const second = assert.commandWorked(
                this.config.adminCommand(this.withSession(cmd)),
            ).changedChunks;
            assertSameChangedChunks(first, second);
            assert.eq(
                0,
                bsonWoCompare(versionAfterFirst, this.getShardVersion(this.shard1)),
                "retry must not bump any version",
            );
        } finally {
            this.setAllowChunkOperations(true);
        }
    });
});
