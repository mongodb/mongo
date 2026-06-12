/**
 * Tests retryable-write replay protection of `_configsvrCommitChunksMerge` on the authoritative
 * path. The configsvr command wraps the catalog-manager call in an AlternativeClientRegion and
 * follows it with a dummy write to NamespaceString::kServerConfigurationNamespace so that an
 * older request on the same session cannot replay the merge onto a newer state.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("_configsvrCommitChunksMerge retryability", function () {
    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;
    const dummyDocId = "commitChunksMergeStats";

    // Range covering the two upper chunks {[0, 100), [100, MaxKey)} so a successful merge
    // collapses them into a single chunk [0, MaxKey).
    const mergeRange = {min: {x: 0}, max: {x: MaxKey}};

    before(function () {
        this.st = new ShardingTest({shards: 1});
        assert.commandWorked(this.st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

        // Pre-split into three chunks: [MinKey, 0), [0, 100), [100, MaxKey).
        assert.commandWorked(this.st.s.adminCommand({split: ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: ns, middle: {x: 100}}));

        const coll = this.st.s.getCollection("config.collections").findOne({_id: ns});
        this.collEpoch = coll.lastmodEpoch;
        this.collTimestamp = coll.timestamp;
        this.collUuid = coll.uuid;
        this.shardName = this.st.shard0.shardName;

        this.lsid = assert.commandWorked(this.st.s.getDB("admin").runCommand({startSession: 1})).id;
    });

    after(function () {
        this.st.stop();
    });

    function runConfigsvrCommitChunksMerge(
        st,
        ns,
        shardName,
        collUuid,
        chunkRange,
        epoch,
        timestamp,
        lsid,
        txnNumber,
    ) {
        let res;
        assert.soon(() => {
            res = st.configRS.getPrimary().adminCommand({
                _configsvrCommitChunksMerge: ns,
                shard: shardName,
                collUUID: {uuid: collUuid},
                chunkRange: chunkRange,
                epoch: epoch,
                timestamp: timestamp,
                lsid: lsid,
                txnNumber: txnNumber,
                writeConcern: {w: "majority"},
            });

            if (
                RetryableWritesUtil.isRetryableCode(res.code) ||
                RetryableWritesUtil.errmsgContainsRetryableCodeName(res.errmsg) ||
                (res.writeConcernError &&
                    RetryableWritesUtil.isRetryableCode(res.writeConcernError.code))
            ) {
                return false;
            }
            return true;
        });
        return res;
    }

    function countChunks(st, uuid) {
        return st.s.getCollection("config.chunks").countDocuments({uuid: uuid});
    }

    function getDummyDoc(st, id) {
        return st.configRS.getPrimary().getDB("admin").system.version.findOne({_id: id});
    }

    it("merges a range in the authoritative branch and records the dummy bookkeeping write", function () {
        assert.eq(countChunks(this.st, this.collUuid), 3, "expected three pre-split chunks", {ns});

        assert.commandWorked(
            runConfigsvrCommitChunksMerge(
                this.st,
                ns,
                this.shardName,
                this.collUuid,
                mergeRange,
                this.collEpoch,
                this.collTimestamp,
                this.lsid,
                NumberLong(1),
            ),
        );

        assert.eq(countChunks(this.st, this.collUuid), 2, "expected two chunks after merge");

        const dummy = getDummyDoc(this.st, dummyDocId);
        assert(dummy, "expected dummy bookkeeping doc to be present", {dummyDocId});
        assert.gte(dummy.count, 1, "expected dummy doc count >= 1", {dummy});
    });

    it("rejects an older txnNumber on the same lsid with TransactionTooOld", function () {
        assert.commandFailedWithCode(
            runConfigsvrCommitChunksMerge(
                this.st,
                ns,
                this.shardName,
                this.collUuid,
                mergeRange,
                this.collEpoch,
                this.collTimestamp,
                this.lsid,
                NumberLong(0),
            ),
            ErrorCodes.TransactionTooOld,
        );
    });

    it("is idempotent when re-invoked with the same lsid and txnNumber", function () {
        const chunkCountBefore = countChunks(this.st, this.collUuid);
        const dummyBefore = getDummyDoc(this.st, dummyDocId);

        assert.commandWorked(
            runConfigsvrCommitChunksMerge(
                this.st,
                ns,
                this.shardName,
                this.collUuid,
                mergeRange,
                this.collEpoch,
                this.collTimestamp,
                this.lsid,
                NumberLong(1),
            ),
        );

        const chunkCountAfter = countChunks(this.st, this.collUuid);
        const dummyAfter = getDummyDoc(this.st, dummyDocId);

        assert.eq(
            chunkCountBefore,
            chunkCountAfter,
            "merge must not be re-applied for the same txnNumber",
            {
                chunkCountBefore,
                chunkCountAfter,
            },
        );
        assert.eq(
            dummyBefore.count,
            dummyAfter.count,
            "dummy bookkeeping count must not advance on a retried txnNumber",
            {dummyBefore, dummyAfter},
        );
    });
});
