// Tests that spelling out the default $merge 'on' for a sharded target (the shard key
// fields plus _id) behaves the same as omitting 'on', even though no unique index covers exactly
// those fields.
// @tags: [
//   requires_fcv_91
// ]

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("$merge with an explicit 'on' equal to the target's document key", function () {
    // 51190 is thrown by mongos, 51183 and 1074330 are the equivalent errors on a shard.
    const kNoUniqueIndexCodes = [51190, 51183, 1074330];

    before(() => {
        this.st = new ShardingTest({shards: 2});
        this.db = this.st.s.getDB("merge_on_explicit_document_key");
        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.db.getName(),
                primaryShard: this.st.shard0.shardName,
            }),
        );
        this.source = this.db.source;
    });

    after(() => {
        this.st.stop();
    });

    // Creates a target sharded on 'key', split at 'splitAt' with the upper chunk on shard1.
    const makeTarget = (name, key, splitAt) => {
        const target = this.db[name];
        target.drop();
        assert.commandWorked(
            this.st.s.adminCommand({shardCollection: target.getFullName(), key: key}),
        );
        assert.commandWorked(
            this.st.s.adminCommand({split: target.getFullName(), middle: splitAt}),
        );
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: target.getFullName(),
                find: splitAt,
                to: this.st.shard1.shardName,
                _waitForDelete: true,
            }),
        );
        return target;
    };

    const merge = (target, extra) => [
        {
            $merge: Object.assign(
                {into: target.getName(), whenMatched: "replace", whenNotMatched: "insert"},
                extra,
            ),
        },
    ];

    const seed = (docs) => {
        this.source.drop();
        assert.commandWorked(this.source.insertMany(docs));
    };

    it("accepts an explicit on that matches default on", () => {
        const target = this.db.coll2;
        target.drop();
        assert.commandWorked(
            this.st.s.adminCommand({shardCollection: target.getFullName(), key: {shardKey: 1}}),
        );
        seed([{_id: 1, shardKey: 1}]);
        assert.commandWorked(target.insert({_id: 1, shardKey: 1, old: true}));

        this.source.aggregate(merge(target, {})).toArray();
        assert.eq([{_id: 1, shardKey: 1}], target.find().toArray());

        assert.commandWorked(target.update({_id: 1}, {_id: 1, shardKey: 1, old: true}));
        // The ticket spelled the fields in the opposite order from the default.
        this.source.aggregate(merge(target, {on: ["_id", "shardKey"]})).toArray();
        assert.eq([{_id: 1, shardKey: 1}], target.find().toArray());
    });

    it("merges across shards and matches existing documents", () => {
        const target = makeTarget("target_a", {a: 1}, {a: 0});
        seed([
            {_id: 1, a: -5, v: "new"},
            {_id: 2, a: 5, v: "new"},
        ]);
        // _id 1 exists under both shard key values, so only (a, _id) identifies a document: the
        // merge must update (1, -5) and leave (1, 5) alone.
        assert.commandWorked(
            target.insertMany([
                {_id: 1, a: -5, v: "old"},
                {_id: 1, a: 5, v: "old"},
                {_id: 2, a: 5, v: "old"},
            ]),
        );

        for (const on of [
            ["a", "_id"],
            ["_id", "a"],
        ]) {
            this.source.aggregate(merge(target, {on: on})).toArray();
            assert.eq(3, target.find().itcount());
            assert.eq(2, target.find({v: "new"}).itcount());
            assert.eq(1, target.find({_id: 1, a: 5, v: "old"}).itcount());
            // Reset to old values for the next ordering.
            assert.commandWorked(target.updateMany({}, {$set: {v: "old"}}));
        }
    });

    it("gives the same result as omitting 'on'", () => {
        const explicitTarget = makeTarget("target_explicit", {a: 1}, {a: 0});
        const defaultTarget = makeTarget("target_default", {a: 1}, {a: 0});
        seed([
            {_id: 1, a: -1},
            {_id: 2, a: 1},
        ]);
        this.source.aggregate(merge(explicitTarget, {on: ["a", "_id"]})).toArray();
        this.source.aggregate(merge(defaultTarget, {})).toArray();
        const sorted = (c) => c.find().sort({_id: 1}).toArray();
        assert.eq(sorted(defaultTarget), sorted(explicitTarget));
    });

    it("supports hashed and compound and dotted shard keys", () => {
        seed([{_id: 1, a: 1, b: {c: 2}}]);

        const hashed = this.db.target_hashed;
        hashed.drop();
        assert.commandWorked(
            this.st.s.adminCommand({shardCollection: hashed.getFullName(), key: {a: "hashed"}}),
        );
        this.source.aggregate(merge(hashed, {on: ["a", "_id"]})).toArray();
        assert.eq(1, hashed.find().itcount());

        const compound = makeTarget("target_compound", {a: 1, _id: 1}, {a: 0, _id: 0});
        // The document key of {a, _id} has no duplicate _id.
        this.source.aggregate(merge(compound, {on: ["a", "_id"]})).toArray();
        assert.eq(1, compound.find().itcount());

        const dotted = makeTarget("target_dotted", {"b.c": 1}, {"b.c": 0});
        this.source.aggregate(merge(dotted, {on: ["b.c", "_id"]})).toArray();
        assert.eq(1, dotted.find().itcount());
    });

    it("compares dotted paths exactly", () => {
        const target = makeTarget("target_dotted_exact", {"b.c": 1}, {"b.c": 0});
        seed([{_id: 1, b: {c: 1}}]);
        // Neither the prefix nor a longer path of the shard key is the document key.
        for (const on of [
            ["b", "_id"],
            ["b.c.d", "_id"],
        ]) {
            assert.throwsWithCode(
                () => this.source.aggregate(merge(target, {on: on})).toArray(),
                kNoUniqueIndexCodes,
            );
        }
    });

    it("still rejects fields that are not the document key", () => {
        const target = makeTarget("target_reject", {a: 1}, {a: 0});
        seed([{_id: 1, a: 1, b: 1}]);
        for (const on of ["a", "_id", ["a", "b"], ["a", "_id", "b"], ["b", "_id"]]) {
            assert.throwsWithCode(
                () => this.source.aggregate(merge(target, {on: on})).toArray(),
                kNoUniqueIndexCodes,
            );
        }
    });

    it("still rejects a collation mismatch with the target", () => {
        const target = this.db.target_collation;
        target.drop();
        assert.commandWorked(
            this.db.createCollection(target.getName(), {collation: {locale: "en_US", strength: 2}}),
        );
        assert.commandWorked(
            this.st.s.adminCommand({
                shardCollection: target.getFullName(),
                key: {a: 1},
                collation: {locale: "simple"},
            }),
        );
        seed([{_id: 1, a: 1}]);
        assert.throwsWithCode(
            () => this.source.aggregate(merge(target, {on: ["a", "_id"]})).toArray(),
            [11749300, 51183],
        );
    });

    it("still rejects the document key spelled out for an unsharded target", () => {
        const target = this.db.target_unsharded;
        target.drop();
        assert.commandWorked(target.insert({_id: 1, a: 1}));
        seed([{_id: 1, a: 1}]);
        assert.throwsWithCode(
            () => this.source.aggregate(merge(target, {on: ["a", "_id"]})).toArray(),
            kNoUniqueIndexCodes,
        );
    });

    it("keeps working when a dedicated unique index exists", () => {
        const target = makeTarget("target_indexed", {a: 1}, {a: 0});
        assert.commandWorked(target.createIndex({a: 1, _id: 1}, {unique: true}));
        seed([{_id: 1, a: 1}]);
        this.source.aggregate(merge(target, {on: ["a", "_id"]})).toArray();
        assert.eq(1, target.find().itcount());
    });

    it("accepts the serialized form of the default 'on' produced by explain", () => {
        const target = makeTarget("target_explain", {a: 1}, {a: 0});
        seed([{_id: 1, a: 1}]);
        // Only executionStats explain resolves the default 'on'; queryPlanner explain skips it.
        const explain = this.source.explain("executionStats").aggregate(merge(target, {}));
        const stages = getAggPlanStages(explain, "$merge");
        assert.gt(stages.length, 0, "expected a $merge stage in explain", {explain});
        const resolvedOn = stages[0].$merge.on;
        assert.sameMembers(
            ["_id", "a"],
            resolvedOn,
            "expected resolved 'on' in explain",
            undefined /* compareFn */,
            {explain},
        );

        // Replay the resolved 'on' the way a user copying the explain output would.
        assert.commandWorked(target.remove({}));
        this.source.aggregate(merge(target, {on: ["_id", "a"]})).toArray();
        assert.eq(1, target.find().itcount());
    });
});
