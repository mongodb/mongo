/**
 * With featureFlagExtensionsInsideHybridSearch enabled, GraphLookUpStage::makePipeline() has a
 * retry loop for CommandOnShardedViewNotSupportedOnMongod kickbacks. If all retries kick back due
 * to a concurrent catalog change (the base collection is replaced by a view mid-execution), the
 * loop exhausts. This test verifies the server converts that situation into a retriable
 * CollectionBecameView error rather than crashing.
 *
 * @tags: [
 *     requires_sharding,
 *     requires_fcv_80,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("$graphLookup view catalog race must not crash the server", function () {
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

        it("resolves the view chain and returns results before any catalog change", function () {
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

        it("returns CollectionBecameView when the backing collection is concurrently replaced by a view", function () {
            if (!FeatureFlagUtil.isEnabled(db, "ExtensionsInsideHybridSearch")) {
                jsTest.log.info("Skipping: featureFlagExtensionsInsideHybridSearch not enabled");
                return;
            }

            // Arm graphLookupStageKickbackFailpoint on every shard node. This pauses execution
            // inside the catch block of makePipeline() after each view-resolution kickback,
            // giving us a window to perform the collection-to-view replacement: dropping 'docs'
            // and recreating it as a view so that every subsequent retry also kicks back.
            const failpoints = FixtureHelpers.mapOnEachShardNode({
                db: db.getSiblingDB("admin"),
                func: (db) => configureFailPoint(db, "graphLookupStageKickbackFailpoint"),
                primaryNodeOnly: false,
            });

            // Run the aggregate in a parallel shell. Once 'docs' has been replaced by a view,
            // every makePipeline() attempt exhausts all retries, and CollectionBecameView
            // propagates to the client.
            const awaitShell = startParallelShell(() => {
                try {
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
                        .toArray();
                } catch (e) {
                    assert.eq(
                        e.code,
                        ErrorCodes.CollectionBecameView,
                        "Expected CollectionBecameView from exhausted retry loop",
                        {error: e},
                    );
                }
            }, sharded.s.port);

            // Wait until at least one shard's failpoint fires: execution is paused inside
            // makePipeline()'s catch block, i.e. we are in the race window.
            assert.soon(
                () => failpoints.some((f) => f.waitWithTimeout(100)),
                "Timed out waiting for graphLookupStageKickbackFailpoint to be hit",
            );

            // Replace 'docs' with a view: every subsequent makePipeline() retry now also kicks
            // back, exhausting the loop.
            db.docs.drop();
            assert.commandWorked(db.createView("docs", db.subjects.getName(), []));

            // Release all failpoints so paused threads can resume.
            failpoints.forEach((f) => f.off());

            awaitShell();

            // Primary invariant: the shard servers must still be alive (no crash).
            assert.commandWorked(
                db.adminCommand({ping: 1}),
                "Shard server should be alive after the view-race scenario",
            );
        });
    });
});
