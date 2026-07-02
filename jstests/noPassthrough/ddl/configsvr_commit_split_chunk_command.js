/**
 * Tests the internal `_configsvrCommitSplitChunk` command. The command commits a chunk split to the
 * global catalog and returns the list of chunks it changed (the new sub-chunks).
 *
 * It is always issued as a retryable write: it requires a session id, runs the commit under an
 * alternative client, and then makes a dummy write so an older request on the same session cannot
 * replay the operation onto a newer state. Each attempt advances the session's txnNumber;
 * idempotency comes from the catalog-level "already split" check, not from retryable-write dedup.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */
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

function getDummyDoc(st, id) {
    return st.configRS.getPrimary().getDB("admin").system.version.findOne({_id: id});
}

describe("_configsvrCommitSplitChunk", function () {
    const dbName = "commit_split_chunk_db";
    const dummyDocId = "commitSplitChunkStats";

    before(function () {
        this.st = new ShardingTest({shards: 1});
        this.config = this.st.configRS.getPrimary().getDB("config");
        this.shard0 = this.st.shard0.shardName;

        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: dbName, primaryShard: this.shard0}),
        );

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

        this.countChunks = () =>
            this.st.s.getCollection("config.chunks").countDocuments({uuid: this.collUuid});

        // Builds the (sessionless) global catalog commit command splitting [min, max) at the given
        // split points. Reads the current shard version, so it must be called before the split is
        // committed; the same cmd is then reused across retries.
        this.buildSplitCmdSessionless = (min, max, splitPoints) => ({
            _configsvrCommitSplitChunk: this.ns,
            shard: this.shard0,
            range: {min, max},
            splitPoints,
            shardVersionPreSplit: chunkVersion(
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
    });

    after(function () {
        this.st.stop();
    });

    beforeEach(function () {
        // Per-test collection so chunk state and the session are isolated.
        this.collName = "coll_" + new ObjectId().str;
        this.ns = dbName + "." + this.collName;

        // Single chunk [MinKey, MaxKey) on shard0.
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));

        const coll = this.st.s.getCollection("config.collections").findOne({_id: this.ns});
        this.collEpoch = coll.lastmodEpoch;
        this.collTimestamp = coll.timestamp;
        this.collUuid = coll.uuid;

        this.lsid = assert.commandWorked(this.st.s.getDB("admin").runCommand({startSession: 1})).id;
        this.txnCounter = 0;
    });

    afterEach(function () {
        assert.commandWorked(this.st.s.getDB(dbName).runCommand({drop: this.collName}));
    });

    it("rejects a call without a session id", function () {
        assert.commandFailedWithCode(
            this.config.adminCommand(
                this.buildSplitCmdSessionless({x: MinKey}, {x: MaxKey}, [{x: 0}]),
            ),
            ErrorCodes.IllegalOperation,
        );
    });

    it("fences a stale txnNumber on the same session", function () {
        const cmd = this.buildSplitCmdSessionless({x: MinKey}, {x: MaxKey}, [{x: 0}]);
        assert.commandWorked(this.config.adminCommand(this.withSession(cmd)));

        // A request that arrives with an older txnNumber than one already committed is rejected.
        assert.commandFailedWithCode(
            this.config.adminCommand({...cmd, lsid: this.lsid, txnNumber: NumberLong(0)}),
            ErrorCodes.TransactionTooOld,
        );
    });

    it("splits the chunk and returns the changed sub-chunks", function () {
        assert.eq(1, this.countChunks(), "expected single initial chunk", {ns: this.ns});

        const cmd = this.buildSplitCmdSessionless({x: MinKey}, {x: MaxKey}, [{x: 0}]);
        const changed = assert.commandWorked(
            this.config.adminCommand(this.withSession(cmd)),
        ).changedChunks;

        assert.eq(2, changed.length, "expected two sub-chunks", {changed});
        const left = findChunk(changed, {x: MinKey}, {x: 0});
        const right = findChunk(changed, {x: 0}, {x: MaxKey});
        assert(left, "left sub-chunk missing", {changed});
        assert(right, "right sub-chunk missing", {changed});
        assert.eq(this.shard0, left.shard, "left sub-chunk not on shard", {left});
        assert.eq(this.shard0, right.shard, "right sub-chunk not on shard", {right});

        assert.eq(2, this.countChunks(), "expected two chunks after split");

        const dummy = getDummyDoc(this.st, dummyDocId);
        assert(dummy, "expected dummy bookkeeping doc to be present", {dummyDocId});
        assert.gte(dummy.count, 1, "expected dummy doc count >= 1", {dummy});
    });

    it("is idempotent when retried after a successful attempt", function () {
        const cmd = this.buildSplitCmdSessionless({x: MinKey}, {x: MaxKey}, [{x: 0}]);

        const first = assert.commandWorked(
            this.config.adminCommand(this.withSession(cmd)),
        ).changedChunks;
        const chunkCountAfterFirst = this.countChunks();
        const versionAfterFirst = this.getShardVersion(this.shard0);

        // The command is retried with an advanced txnNumber; the commit is detected as already
        // applied and returns the same changed chunks.
        const second = assert.commandWorked(
            this.config.adminCommand(this.withSession(cmd)),
        ).changedChunks;

        assertSameChangedChunks(first, second);
        assert.eq(
            chunkCountAfterFirst,
            this.countChunks(),
            "retry must not change the chunk count",
        );
        assert.eq(
            0,
            bsonWoCompare(versionAfterFirst, this.getShardVersion(this.shard0)),
            "retry must not bump any version",
        );
    });

    it("rejects a retry without a session id after a successful attempt", function () {
        const cmd = this.buildSplitCmdSessionless({x: MinKey}, {x: MaxKey}, [{x: 0}]);
        assert.commandWorked(this.config.adminCommand(this.withSession(cmd)));

        // The retry without a session id is still rejected.
        assert.commandFailedWithCode(this.config.adminCommand(cmd), ErrorCodes.IllegalOperation);
    });
});
