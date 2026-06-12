/**
 * Tests retryable-write replay protection of `_configsvrCommitMergeAllChunksOnShard` on the
 * authoritative path. The configsvr command wraps the catalog-manager call in an
 * AlternativeClientRegion and follows it with a dummy write to
 * NamespaceString::kServerConfigurationNamespace so that an older request on the same session
 * cannot replay the merge onto a newer state.
 *
 * The catalog-manager method only merges chunks whose `onCurrentShardSince` is older than the
 * snapshot history window; for freshly-created chunks the test does not assume that a real merge
 * happened. The primary signals under test are the dummy bookkeeping write, the
 * `TransactionTooOld` response for older txnNumbers, and the idempotent return for retried
 * txnNumbers.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("_configsvrCommitMergeAllChunksOnShard retryability", function () {
    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;
    const dummyDocId = "commitMergeAllChunksOnShardStats";

    const kIntMax = NumberInt(2147483647);

    before(function () {
        this.st = new ShardingTest({shards: 1});
        assert.commandWorked(this.st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

        // Pre-split into four chunks on shard0 so that, if the catalog manager finds any of them
        // mergeable, the merge has something to do. The test does not assume a successful merge.
        for (const middle of [{x: 0}, {x: 100}, {x: 200}]) {
            assert.commandWorked(this.st.s.adminCommand({split: ns, middle: middle}));
        }

        const coll = this.st.s.getCollection("config.collections").findOne({_id: ns});
        this.collUuid = coll.uuid;
        this.shardName = this.st.shard0.shardName;

        this.lsid = assert.commandWorked(this.st.s.getDB("admin").runCommand({startSession: 1})).id;
    });

    after(function () {
        this.st.stop();
    });

    function runConfigsvrCommitMergeAllChunksOnShard(st, ns, shardName, lsid, txnNumber) {
        let res;
        assert.soon(() => {
            res = st.configRS.getPrimary().adminCommand({
                _configsvrCommitMergeAllChunksOnShard: ns,
                shard: shardName,
                maxNumberOfChunksToMerge: kIntMax,
                maxTimeProcessingChunksMS: kIntMax,
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

    it("succeeds on the authoritative branch and records the dummy bookkeeping write", function () {
        const chunkCountBefore = countChunks(this.st, this.collUuid);
        assert.gte(chunkCountBefore, 1, "expected at least one pre-split chunk", {ns});

        assert.commandWorked(
            runConfigsvrCommitMergeAllChunksOnShard(
                this.st,
                ns,
                this.shardName,
                this.lsid,
                NumberLong(1),
            ),
        );

        // The catalog manager may merge zero or more chunks depending on the snapshot
        // history window; either way the chunk count must not grow.
        const chunkCountAfter = countChunks(this.st, this.collUuid);
        assert.lte(
            chunkCountAfter,
            chunkCountBefore,
            "chunk count must not grow after mergeAllChunksOnShard",
            {
                chunkCountBefore,
                chunkCountAfter,
            },
        );

        const dummy = getDummyDoc(this.st, dummyDocId);
        assert(dummy, "expected dummy bookkeeping doc to be present", {dummyDocId});
        assert.gte(dummy.count, 1, "expected dummy doc count >= 1", {dummy});
    });

    it("rejects an older txnNumber on the same lsid with TransactionTooOld", function () {
        assert.commandFailedWithCode(
            runConfigsvrCommitMergeAllChunksOnShard(
                this.st,
                ns,
                this.shardName,
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
            runConfigsvrCommitMergeAllChunksOnShard(
                this.st,
                ns,
                this.shardName,
                this.lsid,
                NumberLong(1),
            ),
        );

        const chunkCountAfter = countChunks(this.st, this.collUuid);
        const dummyAfter = getDummyDoc(this.st, dummyDocId);

        assert.eq(
            chunkCountBefore,
            chunkCountAfter,
            "mergeAllChunksOnShard must not be re-applied for the same txnNumber",
            {chunkCountBefore, chunkCountAfter},
        );
        assert.eq(
            dummyBefore.count,
            dummyAfter.count,
            "dummy bookkeeping count must not advance on a retried txnNumber",
            {dummyBefore, dummyAfter},
        );
    });
});
