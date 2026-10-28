/**
 * Tests that $queryStats marks the work resharding performs on the donor shards as isInternal,
 * while leaving user operations routed through mongos unmarked.
 *
 *   1. A user find through mongos produces a shard-side entry with no isInternal field.
 *   2. reshardCollection produces donor-side entries with isInternal: true for both the collection
 *      cloner aggregate and the oplog fetcher aggregate.
 *
 * @tags: [requires_sharding, requires_fcv_91]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * Returns the "aggregate" $queryStats entries on 'conn' for 'db'.'coll'. Entries are not filtered
 * on the shell's application name because entries from resharding carry the recipient's client
 * metadata.
 */
function getAggregateEntries(conn, db, coll) {
    return assert.commandWorked(
        conn.adminCommand({
            aggregate: 1,
            pipeline: [
                {$queryStats: {}},
                {
                    $match: {
                        "key.queryShape.command": "aggregate",
                        "key.queryShape.cmdNs.db": db,
                        "key.queryShape.cmdNs.coll": coll,
                    },
                },
            ],
            cursor: {},
        }),
    ).cursor.firstBatch;
}

describe("query stats isInternal for resharding", function () {
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
        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(
            coll.insert([
                {_id: 1, v: 1},
                {_id: 2, v: 2},
                {_id: 3, v: 3},
            ]),
        );
    });

    after(function () {
        st?.stop();
    });

    it("user find through mongos is NOT marked isInternal on the shard", function () {
        resetQueryStatsStore(st.shard0, "1MB");
        resetQueryStatsStore(st.shard1, "1MB");

        assert.eq(coll.find({_id: 1}).toArray().length, 1);

        const entries = assert.commandWorked(
            st.shard0.adminCommand({
                aggregate: 1,
                pipeline: [
                    {$queryStats: {}},
                    {
                        $match: {
                            "key.queryShape.command": "find",
                            "key.queryShape.cmdNs.coll": collName,
                        },
                    },
                ],
                cursor: {},
            }),
        ).cursor.firstBatch;

        assert.eq(entries.length, 1, "Expected exactly 1 find query stats entry on shard0", {
            entries,
        });
        assert.eq(
            entries[0].key.isInternal,
            undefined,
            "User find through mongos must NOT be marked isInternal on the shard",
            {entry: entries[0]},
        );
    });

    it("reshardCollection donor entries ARE marked isInternal", function () {
        resetQueryStatsStore(st.shard0, "1MB");
        resetQueryStatsStore(st.shard1, "1MB");

        assert.commandWorked(
            st.s.adminCommand({
                reshardCollection: coll.getFullName(),
                key: {v: 1},
                numInitialChunks: 2,
            }),
        );

        // The recipient stamps its requests to the donor only once the cluster's FCV enables the
        // flag; some suites start at the last LTS FCV.
        const expectedIsInternal = FeatureFlagUtil.isEnabled(st.shard0, "InternalOperationOrigin")
            ? true
            : undefined;

        // With {_id: 1} sharding and no splits, shard0 is the only donor.
        const clonerEntries = getAggregateEntries(st.shard0, testDB.getName(), collName).filter(
            (e) => JSON.stringify(e.key.queryShape).includes("_internalReshardingOwnershipMatch"),
        );
        assert.gte(
            clonerEntries.length,
            1,
            "Expected at least 1 resharding collection cloner aggregate entry on the donor shard",
        );
        for (const entry of clonerEntries) {
            assert.eq(
                entry.key.isInternal,
                expectedIsInternal,
                "Resharding collection cloner aggregate should be marked isInternal",
                {entry},
            );
        }

        // FTDC also runs $collStats on local.oplog.rs, so match the fetcher by its pipeline.
        const fetcherEntries = getAggregateEntries(st.shard0, "local", "oplog.rs").filter((e) =>
            JSON.stringify(e.key.queryShape).includes("_addReshardingResumeId"),
        );
        assert.gte(
            fetcherEntries.length,
            1,
            "Expected at least 1 resharding oplog fetcher aggregate entry on the donor shard",
        );
        for (const entry of fetcherEntries) {
            assert.eq(
                entry.key.isInternal,
                expectedIsInternal,
                "Resharding oplog fetcher aggregate should be marked isInternal",
                {entry},
            );
        }
    });
});
