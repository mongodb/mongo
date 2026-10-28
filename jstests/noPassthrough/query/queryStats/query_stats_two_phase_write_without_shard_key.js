/**
 * Tests that $queryStats does not mark the shard-side sub-operations of the two-phase "write
 * without shard key" protocol as isInternal. A single-document update or findAndModify whose filter
 * lacks the shard key is executed by mongos through _clusterQueryWithoutShardKey and
 * _clusterWriteWithoutShardKey. Those commands carry out the client's write, so they are not
 * internal.
 *
 * @tags: [requires_sharding, requires_fcv_91]
 */
import {before, after, describe, it} from "jstests/libs/mochalite.js";
import {resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * Not filtered on the shell's application name: the shard-side entries carry mongos's client
 * metadata.
 */
function getQueryStatsForColl(conn, collName) {
    return assert.commandWorked(
        conn.adminCommand({
            aggregate: 1,
            pipeline: [{$queryStats: {}}, {$match: {"key.queryShape.cmdNs.coll": collName}}],
            cursor: {},
        }),
    ).cursor.firstBatch;
}

describe("query stats isInternal for writes without the shard key", function () {
    let st;
    let testDB;
    let coll;
    const collName = jsTestName();

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongosOptions: {
                setParameter: {
                    internalQueryStatsSampleRate: 1,
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
            },
            rsOptions: {
                setParameter: {
                    internalQueryStatsSampleRate: 1,
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
            },
        });
        testDB = st.s.getDB("test");
        coll = testDB[collName];

        assert.commandWorked(
            testDB.adminCommand({
                enableSharding: testDB.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );
        // Shard on {x: 1} and spread the data over both shards so that a filter without the shard
        // key cannot be targeted to a single shard.
        assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
        assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {x: 0}}));
        assert.commandWorked(
            st.s.adminCommand({
                moveChunk: coll.getFullName(),
                find: {x: 5},
                to: st.shard1.shardName,
            }),
        );
        assert.commandWorked(
            coll.insert([
                {_id: 1, x: -1, y: 1},
                {_id: 2, x: 5, y: 5},
            ]),
        );
    });

    after(function () {
        st?.stop();
    });

    it("updateOne without the shard key is not marked isInternal on the shards", function () {
        resetQueryStatsStore(st.s, "1MB");
        resetQueryStatsStore(st.shard0, "1MB");
        resetQueryStatsStore(st.shard1, "1MB");

        // {y: 5} does not contain the shard key {x: 1}, so this goes through the two-phase
        // write-without-shard-key protocol. The document lives on shard1.
        assert.commandWorked(coll.updateOne({y: 5}, {$set: {z: 1}}));

        // Mongos registers the user's update on the user's own operation context, so it is
        // correctly not marked internal.
        const mongosEntries = getQueryStatsForColl(st.s, collName).filter(
            (e) => e.key.queryShape.command === "update",
        );
        assert.eq(mongosEntries.length, 1, "Expected exactly 1 update entry on mongos", {
            mongosEntries,
        });
        assert.eq(
            mongosEntries[0].key.isInternal,
            undefined,
            "User update on mongos must NOT be marked isInternal",
            {entry: mongosEntries[0]},
        );

        // Phase one broadcasts an aggregate built from the user's filter to every shard that owns
        // chunks, looking for a document to write.
        for (const shard of [st.shard0, st.shard1]) {
            const aggEntries = getQueryStatsForColl(shard, collName).filter(
                (e) => e.key.queryShape.command === "aggregate",
            );
            assert.eq(
                aggEntries.length,
                1,
                "Expected exactly 1 aggregate entry from phase one of the two-phase protocol",
                {shard: shard.name, aggEntries},
            );
            assert.docEq(
                aggEntries[0].key.queryShape.pipeline,
                [{$match: {y: {$eq: "?number"}}}, {$limit: "?number"}, {$project: {_id: true}}],
                "Phase-one aggregate should carry the user's filter",
            );
            assert.eq(
                aggEntries[0].key.isInternal,
                undefined,
                "Phase-one aggregate executes the user's update and must NOT be marked isInternal",
                {entry: aggEntries[0]},
            );
        }

        // Phase two runs the user's update against the shard that owns the matched document, with
        // the filter rewritten to target it by _id.
        const shardUpdateEntries = getQueryStatsForColl(st.shard1, collName).filter(
            (e) => e.key.queryShape.command === "update",
        );
        assert.eq(shardUpdateEntries.length, 1, "Expected exactly 1 update entry on shard1", {
            shardUpdateEntries,
        });
        assert.docEq(
            shardUpdateEntries[0].key.queryShape,
            {
                command: "update",
                cmdNs: {db: testDB.getName(), coll: collName},
                q: {_id: {$eq: "?number"}},
                u: {$set: {z: "?number"}},
                multi: false,
                upsert: false,
            },
            "Phase-two update should carry the user's modifier, retargeted by _id",
        );
        assert.eq(
            shardUpdateEntries[0].key.isInternal,
            undefined,
            "Phase-two update is the user's own write and must NOT be marked isInternal",
            {entry: shardUpdateEntries[0]},
        );
    });

    it("the same updateOne with the shard key is not marked isInternal", function () {
        resetQueryStatsStore(st.s, "1MB");
        resetQueryStatsStore(st.shard0, "1MB");
        resetQueryStatsStore(st.shard1, "1MB");

        // {x: 5} contains the full shard key, so mongos targets shard1 directly from the user's
        // operation context.
        assert.commandWorked(coll.updateOne({x: 5}, {$set: {z: 2}}));

        const shardUpdateEntries = getQueryStatsForColl(st.shard1, collName).filter(
            (e) => e.key.queryShape.command === "update",
        );
        assert.eq(shardUpdateEntries.length, 1, "Expected exactly 1 update entry on shard1", {
            shardUpdateEntries,
        });
        assert.eq(
            shardUpdateEntries[0].key.isInternal,
            undefined,
            "A targeted user update must NOT be marked isInternal on the shard",
            {entry: shardUpdateEntries[0]},
        );
        // No two-phase protocol, so no phase-one aggregate is issued at all.
        assert.eq(
            getQueryStatsForColl(st.shard0, collName).length,
            0,
            "The non-owning shard should see no entries for a targeted update",
        );
    });

    // Checks the phase-one aggregate the shards run for the findAndModify.
    it("findAndModify without the shard key is not marked isInternal on the shards", function () {
        resetQueryStatsStore(st.shard0, "1MB");
        resetQueryStatsStore(st.shard1, "1MB");

        assert.commandWorked(
            testDB.runCommand({findAndModify: collName, query: {y: 5}, update: {$set: {w: 1}}}),
        );

        for (const shard of [st.shard0, st.shard1]) {
            const aggEntries = getQueryStatsForColl(shard, collName).filter(
                (e) => e.key.queryShape.command === "aggregate",
            );
            assert.eq(
                aggEntries.length,
                1,
                "Expected exactly 1 aggregate entry from phase one of the two-phase protocol",
                {shard: shard.name, aggEntries},
            );
            assert.eq(
                aggEntries[0].key.isInternal,
                undefined,
                "Phase-one aggregate of a user's findAndModify must NOT be marked isInternal",
                {entry: aggEntries[0]},
            );
        }
    });
});
