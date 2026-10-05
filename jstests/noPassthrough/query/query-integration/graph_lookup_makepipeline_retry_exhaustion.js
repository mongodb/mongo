/**
 * With featureFlagExtensionsInsideHybridSearch enabled, GraphLookUpStage::makePipeline() retries on
 * CommandOnShardedViewNotSupportedOnMongod kickbacks and, when every attempt kicks back, surfaces a
 * retriable error to the client instead of crashing. This test drives that exhaustion path with
 * graphLookupStageKickbackFailpoint, which forces makePipeline() to kick back on every attempt
 * wherever the stage runs, and asserts a retriable error is returned and no node crashes.
 *
 * @tags: [
 *     requires_sharding,
 *     requires_fcv_90,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("$graphLookup makePipeline retry exhaustion must not crash the server", function () {
    let sharded;
    let db;

    before(function () {
        sharded = new ShardingTest({
            mongos: 1,
            shards: [{}, {}],
            config: 1,
            other: {
                mongosOptions: {setParameter: {featureFlagExtensionsInsideHybridSearch: true}},
                shardOptions: {setParameter: {featureFlagExtensionsInsideHybridSearch: true}},
                configOptions: {setParameter: {featureFlagExtensionsInsideHybridSearch: true}},
            },
        });
        db = sharded.getDB("test");
        assert.commandWorked(
            sharded.s.adminCommand({
                enableSharding: "test",
                primaryShard: sharded.shard0.shardName,
            }),
            "failed to enable sharding",
        );
    });

    after(function () {
        sharded.stop();
    });

    describe("physicistsView $graphLookup-ing into a sharded subjectsView", function () {
        before(function () {
            if (!FeatureFlagUtil.isEnabled(db, "ExtensionsInsideHybridSearch")) {
                return;
            }

            // Set up 'docs' on shard0.
            const docs = db.docs;
            docs.drop();
            assert.commandWorked(docs.createIndex({shard_key: 1}));
            assert.commandWorked(
                docs.insertMany([
                    {_id: 1, shard_key: "s0", name: "Carter", subject: "Astrophysics"},
                    {_id: 2, shard_key: "s0", name: "Jackson", subject: "Archaeology"},
                ]),
            );
            assert.commandWorked(
                sharded.s.adminCommand({shardCollection: docs.getFullName(), key: {shard_key: 1}}),
            );
            assert.commandWorked(
                sharded.s.adminCommand({
                    moveChunk: docs.getFullName(),
                    find: {shard_key: "s0"},
                    to: sharded.shard0.shardName,
                }),
            );

            // Set up 'subjects' on shard1.
            const subjects = db.subjects;
            subjects.drop();
            assert.commandWorked(subjects.createIndex({shard_key: 1}));
            assert.commandWorked(
                subjects.insertMany([
                    {_id: 1, shard_key: "s1", name: "Science"},
                    {_id: 2, shard_key: "s1", name: "Physics", parent: "Science"},
                    {_id: 3, shard_key: "s1", name: "Astrophysics", parent: "Physics"},
                    {_id: 4, shard_key: "s1", name: "Anthropology"},
                    {_id: 5, shard_key: "s1", name: "Archaeology", parent: "Anthropology"},
                ]),
            );
            assert.commandWorked(
                sharded.s.adminCommand({
                    shardCollection: subjects.getFullName(),
                    key: {shard_key: 1},
                }),
            );
            assert.commandWorked(
                sharded.s.adminCommand({
                    moveChunk: subjects.getFullName(),
                    find: {shard_key: "s1"},
                    to: sharded.shard1.shardName,
                }),
            );

            // Simple view over subjects.
            assert.commandWorked(db.createView("subjectsView", subjects.getName(), []));

            // physicistsView: backed by docs, with a $graphLookup referencing subjectsView.
            assert.commandWorked(
                db.createView("physicistsView", docs.getName(), [
                    {
                        $graphLookup: {
                            from: "subjectsView",
                            startWith: "$subject",
                            connectFromField: "parent",
                            connectToField: "name",
                            as: "subjects",
                        },
                    },
                    {$project: {name: 1, subjects: "$subjects.name"}},
                ]),
            );
        });

        it("resolves the view chain and returns results without forced retry exhaustion", function () {
            if (!FeatureFlagUtil.isEnabled(db, "ExtensionsInsideHybridSearch")) {
                jsTest.log.info("Skipping: featureFlagExtensionsInsideHybridSearch not enabled");
                return;
            }

            const results = db.subjects
                .aggregate([
                    {
                        $graphLookup: {
                            from: "physicistsView",
                            startWith: "$name",
                            connectFromField: "subjects",
                            connectToField: "name",
                            as: "practitioners",
                        },
                    },
                ])
                .toArray();
            assert.gt(results.length, 0, "expected at least one result from view chain");
        });

        it("does not crash and surfaces a retriable error when the foreign view resolution never settles", function () {
            if (!FeatureFlagUtil.isEnabled(db, "ExtensionsInsideHybridSearch")) {
                jsTest.log.info("Skipping: featureFlagExtensionsInsideHybridSearch not enabled");
                return;
            }

            // Force makePipeline() to kick back on every attempt, driving the stage's retry loop
            // (and the mongos view-retry loop) to exhaustion. The stage runs on a shard node or the
            // merging mongos, so arm the failpoint on both.
            const failpoints = FixtureHelpers.mapOnEachShardNode({
                db: db.getSiblingDB("admin"),
                func: (db) => configureFailPoint(db, "graphLookupStageKickbackFailpoint"),
                primaryNodeOnly: false,
            });
            failpoints.push(
                configureFailPoint(sharded.s.getDB("admin"), "graphLookupStageKickbackFailpoint"),
            );

            try {
                // Exhaustion surfaces a retriable error, never a crash. makePipeline() re-throws
                // CommandOnShardedViewNotSupportedOnMongod; mongos may surface that or
                // CollectionBecameView after its own view retries. Either is acceptable.
                const err = assert.throws(
                    () =>
                        db.subjects
                            .aggregate([
                                {
                                    $graphLookup: {
                                        from: "physicistsView",
                                        startWith: "$name",
                                        connectFromField: "subjects",
                                        connectToField: "name",
                                        as: "practitioners",
                                    },
                                },
                            ])
                            .toArray(),
                    [],
                    "expected the exhausted retry loop to surface an error",
                );
                assert.contains(
                    err.code,
                    [
                        ErrorCodes.CollectionBecameView,
                        ErrorCodes.CommandOnShardedViewNotSupportedOnMongod,
                    ],
                    "expected a retriable view-resolution error from the exhausted retry loop",
                    {error: err},
                );
            } finally {
                failpoints.forEach((f) => f.off());
            }

            // No node crashed. mongos stays up even if a shard mongod dies, so ping every shard
            // node (where the stage runs) as well as mongos.
            assert.commandWorked(
                db.adminCommand({ping: 1}),
                "mongos should be alive after retry-loop exhaustion",
            );
            FixtureHelpers.mapOnEachShardNode({
                db: db.getSiblingDB("admin"),
                func: (nodeDb) =>
                    assert.commandWorked(
                        nodeDb.adminCommand({ping: 1}),
                        "shard node should be alive after retry-loop exhaustion",
                    ),
                primaryNodeOnly: false,
            });
        });
    });
});
