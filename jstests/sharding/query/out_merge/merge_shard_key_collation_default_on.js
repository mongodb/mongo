/**
 * Tests that $merge respects the shard-key collation (not just the _id collation) when the "on"
 * field defaults to the implicit "_id + shard key" set.
 *
 * It is legal for the shard-key collation to differ from the collection's default collation: the
 * shard-key collation is used for routing while the collection default collation is used for
 * uniqueness / matching. SERVER-117493 began checking the _id collation against the source
 * collation. SERVER-120752 extends that check to the shard-key fields: if the shard-key fields'
 * collation does not match the source collation, $merge with the default "on" must fail with the
 * collation-mismatch error rather than silently producing a misrouted write.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("$merge default 'on' must respect shard key collation", function () {
    const kCollationMismatch = 51183;

    const kCaseInsensitive = {locale: "en_US", strength: 2};
    const kSimple = {locale: "simple"};

    before(() => {
        this.st = new ShardingTest({shards: 2});
        this.dbName = "merge_shard_key_collation_default_on";
        this.db = this.st.s0.getDB(this.dbName);
        assert.commandWorked(
            this.db.adminCommand({enableSharding: this.dbName, primaryShard: this.st.shard0.shardName}),
        );
    });

    after(() => {
        this.st.stop();
    });

    /**
     * Create a destination collection whose default collation is `collCollation` and whose
     * shard-key index has collation `shardKeyCollation`. The shard key is {sk: 1} so the
     * implicit $merge "on" becomes ["_id", "sk"], which is what this ticket targets.
     */
    function createShardedDest(testCtx, name, collCollation, shardKeyCollation) {
        const coll = testCtx.db[name];
        coll.drop();
        const createCmd = {create: coll.getName()};
        if (collCollation) {
            createCmd.collation = collCollation;
        }
        assert.commandWorked(testCtx.db.runCommand(createCmd));
        assert.commandWorked(
            testCtx.db.adminCommand({
                shardCollection: coll.getFullName(),
                key: {sk: 1},
                collation: shardKeyCollation,
            }),
        );
        return coll;
    }

    function createSource(testCtx, name, collation) {
        const coll = testCtx.db[name];
        coll.drop();
        const createCmd = {create: coll.getName()};
        if (collation) {
            createCmd.collation = collation;
        }
        assert.commandWorked(testCtx.db.runCommand(createCmd));
        assert.commandWorked(coll.insertOne({_id: 0, sk: "abc", payload: "from_source"}));
        return coll;
    }

    function runDefaultOnMerge(srcColl, dstColl) {
        return srcColl.getDB().runCommand({
            aggregate: srcColl.getName(),
            pipeline: [
                {
                    $merge: {
                        into: dstColl.getName(),
                        whenMatched: "replace",
                        whenNotMatched: "insert",
                    },
                },
            ],
            cursor: {},
        });
    }

    it("fails when destination shard-key collation differs from source collation", () => {
        // Destination: collection default = caseInsensitive (matches what _id check would pass),
        // shard-key {sk: 1} pinned to the simple collation. Source: caseInsensitive.
        // After SERVER-117493 the _id leg would pass; the shard-key leg must now reject.
        const dst = createShardedDest(this, "dst_collIns_skSimple", kCaseInsensitive, kSimple);
        const src = createSource(this, "src_collIns", kCaseInsensitive);

        const res = runDefaultOnMerge(src, dst);
        assert.commandFailedWithCode(res, kCollationMismatch);
    });

    it("fails when destination collation matches but shard-key collation does not", () => {
        // Destination: collection default = simple, shard-key {sk: 1} = caseInsensitive.
        // Source: simple. _id check would pass; shard-key check must reject.
        const dst = createShardedDest(this, "dst_collSimple_skIns", kSimple, kCaseInsensitive);
        const src = createSource(this, "src_collSimple", kSimple);

        const res = runDefaultOnMerge(src, dst);
        assert.commandFailedWithCode(res, kCollationMismatch);
    });

    it("succeeds when destination collection and shard-key collations both match source", () => {
        // All three collations aligned (simple). Default "on" = ["_id", "sk"] is safe.
        const dst = createShardedDest(this, "dst_all_simple", kSimple, kSimple);
        const src = createSource(this, "src_all_simple", kSimple);

        assert.commandWorked(runDefaultOnMerge(src, dst));
        assert.eq(1, dst.find({_id: 0, sk: "abc"}).itcount());
    });

    it("succeeds when all three collations are case-insensitive", () => {
        // Equivalent to the previous case, but for a non-simple collation: the shard-key index
        // collation and collection default both equal the source collation, so default-on $merge
        // must not be rejected by the new shard-key check.
        const dst = createShardedDest(this, "dst_all_ins", kCaseInsensitive, kCaseInsensitive);
        const src = createSource(this, "src_all_ins", kCaseInsensitive);

        assert.commandWorked(runDefaultOnMerge(src, dst));
        assert.eq(1, dst.find({_id: 0, sk: "abc"}).collation(kCaseInsensitive).itcount());
    });

    it("fails when an explicit default-equivalent 'on' is supplied with mismatched shard-key collation", () => {
        // Same mismatch as the first case, but caller writes out the implicit list explicitly.
        // The shard-key check must fire identically regardless of whether "on" was defaulted or
        // listed verbatim - the bug is in the collation check, not in the defaulting code path.
        const dst = createShardedDest(this, "dst_explicit_on", kCaseInsensitive, kSimple);
        const src = createSource(this, "src_explicit_on", kCaseInsensitive);

        const res = src.getDB().runCommand({
            aggregate: src.getName(),
            pipeline: [
                {
                    $merge: {
                        into: dst.getName(),
                        on: ["_id", "sk"],
                        whenMatched: "replace",
                        whenNotMatched: "insert",
                    },
                },
            ],
            cursor: {},
        });
        assert.commandFailedWithCode(res, kCollationMismatch);
    });
});
