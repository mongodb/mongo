/**
 * Reproduces SERVER-134439: $graphLookup views synced across shards break when
 * featureFlagExtensionsInsideHybridSearch is disabled.
 *
 * Before the fix, reading such a view failed with Location13248900 instead of returning results.
 *
 * A last-lts (< 9.0) mongos sends no ifrFlags, so the 9.0 shards install the flag as disabled. When
 * the flag-disabled shard re-serializes a $graphLookup sub-pipeline for a remote shard, it must
 * re-resolve the referenced views by name instead of emitting $_internalFromPipeline.
 *
 * On branches where last-lts is already 9.0, the last-lts mongos sends ifrFlags with the flag
 * enabled; this test then passes regardless of the fix and acts as a regression guard.
 *
 * TODO SERVER-121094 consider re-homing or generalizing this test once the feature flag is removed.
 *
 * @tags: [ ]
 *
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("$graphLookup views across shards when the hybrid-search flag is disabled", function () {
    before(function () {
        // last-lts mongos over latest shards. On master, last-lts is 9.0, so the mongos sends
        // ifrFlags with featureFlagExtensionsInsideHybridSearch enabled.
        // TODO BACKPORT-29702: update this comment to document that last-lts is 8.0 and this test
        // exercises the flag-disabled serialization path.
        this.st = new ShardingTest({
            shards: [{binVersion: "latest"}, {binVersion: "latest"}],
            mongos: 1,
            other: {mongosOptions: {binVersion: "last-lts"}},
        });

        const db = this.st.s.getDB(jsTestName());
        this.db = db;
        this.coll = db.coll;

        // Spread the graph across both shards so the $graphLookup sub-pipeline sub-routes from one
        // shard to the other (the serialization that emits $_internalFromPipeline). shardColl moves
        // the {_id: 1} chunk to the non-primary shard.
        this.st.shardColl(this.coll, {_id: 1}, {_id: 0}, {_id: 1});
        assert.commandWorked(
            this.coll.insertMany([
                {_id: -1, name: "Alice", friends: ["Bob"]}, // stays on the primary shard
                {_id: 1, name: "Bob", friends: ["Alice"]}, // moved to the other shard
            ]),
        );

        // A chain of nested-view pipelines that the traversing sub-pipelines must carry across
        // shards. viewA applies a transforming stage; viewB and viewD each run a $graphLookup into
        // the view below them.
        assert.commandWorked(
            db.createView("viewA", this.coll.getName(), [{$addFields: {fromViewA: true}}]),
        );
        assert.commandWorked(
            db.createView("viewB", this.coll.getName(), [
                {
                    $graphLookup: {
                        from: "viewA",
                        startWith: "$name",
                        connectFromField: "friends",
                        connectToField: "name",
                        as: "nested",
                    },
                },
            ]),
        );
        assert.commandWorked(
            db.createView("viewD", this.coll.getName(), [
                {
                    $graphLookup: {
                        from: "viewB",
                        startWith: "$name",
                        connectFromField: "friends",
                        connectToField: "name",
                        as: "connections",
                    },
                },
            ]),
        );
    });

    after(function () {
        this.st.stop();
    });

    it("traverses the graph to a node on each shard", function () {
        const doc = this.db.viewD.findOne({_id: -1});
        assert(doc, "expected viewD to return a result");

        const connections = doc.connections.map((c) => c.name).sort();
        assert.eq(
            connections,
            ["Alice", "Bob"],
            "expected the traversal to reach both Alice and Bob",
            {doc},
        );
    });

    it("preserves the nested view pipelines on the flag-disabled shard", function () {
        const doc = this.db.viewD.findOne({_id: -1});
        assert(doc, "expected viewD to return a result");

        // viewB's pipeline runs a nested $graphLookup into viewA. If the flag-disabled receiver
        // dropped the view pipeline (e.g. rewrote 'from' to the backing collection), 'nested' would
        // be absent and viewA's {fromViewA: true} marker would be missing.
        assert.gt(doc.connections.length, 0, "expected viewD to have connections");
        for (const connection of doc.connections) {
            assert(
                Array.isArray(connection.nested),
                "expected viewB's pipeline to run its nested $graphLookup against viewA",
                {doc},
            );
            assert.gt(
                connection.nested.length,
                0,
                "expected viewB's nested $graphLookup to return results",
            );
            for (const n of connection.nested) {
                assert.eq(n.fromViewA, true, "viewA's $addFields pipeline must be applied", {doc});
            }
        }
    });

    it("does not leak $_internalFromPipeline in explain", function () {
        const explain = this.db.viewD.explain().aggregate();

        // The explain-suppression of $_internalFromPipeline in $graphLookup landed on master after
        // 9.0 branched (SERVER-134439). On master, last-lts is 9.0, so the last-lts mongos
        // producing this explain still serializes $_internalFromPipeline in its top-level 'command'
        // field. The 'latest' shards run the patched code and must not leak the field in their
        // shard-side explain stages. On the v9.0 backport (last-lts 8.0) the mongos does not emit
        // the field at all, so the whole-document absence assertion holds.
        // TODO(SERVER-134527): once $graphLookup reports view resolution consistently in explain,
        // cover the master mongos suppression directly with a 'latest' mongos.
        if (MongoRunner.compareBinVersions("last-lts", "9.0") >= 0) {
            for (const shardName of Object.keys(explain.shards)) {
                const shardExplain = explain.shards[shardName];
                assert(
                    !tojson(shardExplain).includes("$_internalFromPipeline"),
                    "shard explain should not contain $_internalFromPipeline",
                    {shard: shardName, shardExplain},
                );
            }
            return;
        }

        assert(
            !tojson(explain).includes("$_internalFromPipeline"),
            "explain output should not contain $_internalFromPipeline",
            {explain},
        );
    });
});
