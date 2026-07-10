/**
 * Tests serverStatus metrics under metrics.query.nonLeadingPushdown.
 *
 * @tags: [
 *   requires_sbe,
 *   incompatible_with_join_optimization,
 *   featureFlagGetExecutorDeferredEngineChoice,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

function getCounters(db) {
    return db.serverStatus().metrics.query.nonLeadingPushdown;
}

describe("nonLeadingPushdown metrics", function () {
    let conn, db, coll, foreign;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                internalQueryFrameworkControl: "trySbeRestricted",
                featureFlagSbeFull: false,
                featureFlagSbeNonLeadingMatch: true,
                featureFlagSbeTransformStages: true,
            },
        });
        assert.neq(null, conn);
        db = conn.getDB(jsTestName());
        coll = db.coll;
        foreign = db.foreign;
        coll.drop();
        foreign.drop();
        for (let i = 0; i < 5; i++) {
            assert.commandWorked(coll.insert({_id: i, x: i % 3, a: i}));
            assert.commandWorked(foreign.insert({_id: i, b: i}));
        }
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("increments match for $group + $match", function () {
        const before = getCounters(db);
        coll.aggregate([
            {$group: {_id: null, s: {$sum: "$x"}}},
            {$match: {s: {$gt: 0}}}, // $match has to depend on a field from $group, otherwise it's pushed down to the find layer.
        ]).toArray();
        const after = getCounters(db);
        assert.eq(after.match - before.match, 1);
        assert.eq(after.project - before.project, 0);
        assert.eq(after.addFields - before.addFields, 0);
        assert.eq(after.replaceRoot - before.replaceRoot, 0);
    });

    it("increments project for $group + $project", function () {
        const before = getCounters(db);
        coll.aggregate([
            {$group: {_id: "$x", total: {$sum: 1}}},
            {$project: {_id: 0, total: 1}},
        ]).toArray();
        const after = getCounters(db);
        assert.eq(after.match - before.match, 0);
        assert.eq(after.project - before.project, 1);
        assert.eq(after.addFields - before.addFields, 0);
        assert.eq(after.replaceRoot - before.replaceRoot, 0);
    });

    it("increments addFields for $group + $addFields", function () {
        const before = getCounters(db);
        coll.aggregate([{$group: {_id: "$x"}}, {$addFields: {label: "a"}}]).toArray();
        const after = getCounters(db);
        assert.eq(after.match - before.match, 0);
        assert.eq(after.project - before.project, 0);
        assert.eq(after.addFields - before.addFields, 1);
        assert.eq(after.replaceRoot - before.replaceRoot, 0);
    });

    it("increments replaceRoot for $group + $replaceRoot", function () {
        const before = getCounters(db);
        coll.aggregate([
            {$group: {_id: "$x", total: {$sum: 1}}},
            {$replaceRoot: {newRoot: {count: "$total"}}},
        ]).toArray();
        const after = getCounters(db);
        assert.eq(after.match - before.match, 0);
        assert.eq(after.project - before.project, 0);
        assert.eq(after.addFields - before.addFields, 0);
        assert.eq(after.replaceRoot - before.replaceRoot, 1);
    });

    it("increments match for $lookup + $match", function () {
        const before = getCounters(db);
        coll.aggregate([
            {
                $lookup: {
                    from: foreign.getName(),
                    localField: "a",
                    foreignField: "b",
                    as: "res",
                },
            },
            {$match: {res: {$ne: []}}},
        ]).toArray();
        const after = getCounters(db);
        assert.eq(after.match - before.match, 1);
        assert.eq(after.project - before.project, 0);
        assert.eq(after.addFields - before.addFields, 0);
        assert.eq(after.replaceRoot - before.replaceRoot, 0);
    });

    it("increments all counters when all stage types present", function () {
        const before = getCounters(db);
        coll.aggregate([
            {$group: {_id: "$x", total: {$sum: 1}}},
            {$match: {total: {$gt: 0}}},
            {$project: {_id: 0, total: 1}},
            {$addFields: {label: "a"}},
            {$replaceRoot: {newRoot: {count: "$total", label: "$label"}}},
        ]).toArray();
        const after = getCounters(db);
        assert.eq(after.match - before.match, 1);
        assert.eq(after.project - before.project, 1);
        assert.eq(after.addFields - before.addFields, 1);
        assert.eq(after.replaceRoot - before.replaceRoot, 1);
    });

    it("increments by 2 when same pipeline runs twice", function () {
        const pipeline = [{$group: {_id: null, s: {$sum: "$x"}}}, {$match: {sum: {$gt: 0}}}]; // $match has to depend on a field from $group, otherwise it's pushed down to the find layer.
        const before = db.serverStatus().metrics.query.nonLeadingPushdown;
        coll.aggregate(pipeline).toArray();
        coll.aggregate(pipeline).toArray();
        const after = db.serverStatus().metrics.query.nonLeadingPushdown;
        assert.eq(after.match - before.match, 2);
    });

    it("does not increment for plain find", function () {
        const before = getCounters(db);
        coll.find({x: {$gt: 0}}).toArray();
        const after = getCounters(db);
        assert.eq(after.match - before.match, 0);
        assert.eq(after.project - before.project, 0);
        assert.eq(after.addFields - before.addFields, 0);
        assert.eq(after.replaceRoot - before.replaceRoot, 0);
    });

    it("increments match only once when multiple non-leading $match stages are present", function () {
        const before = getCounters(db);
        coll.aggregate([
            {$group: {_id: "$x", total: {$sum: 1}}},
            {$match: {total: {$gt: 0}}},
            // $project computes "squared" so the second $match cannot be pushed past it and
            // merged with the first.
            {$project: {_id: 0, total: 1, squared: {$multiply: ["$total", "$total"]}}},
            {$match: {squared: {$lt: 100}}},
        ]).toArray();
        const after = getCounters(db);
        assert.eq(after.match - before.match, 1);
        assert.eq(after.project - before.project, 1);
        assert.eq(after.addFields - before.addFields, 0);
        assert.eq(after.replaceRoot - before.replaceRoot, 0);
    });

    it("does not increment addFields when $LU is ineligible due to collscan", function () {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                featureFlagSbeEqLookupUnwindLocalCollscan: false,
            }),
        );
        try {
            const before = getCounters(db);
            coll.aggregate([
                {$group: {_id: "$x", total: {$sum: 1}}},
                {
                    $lookup: {
                        from: foreign.getName(),
                        localField: "_id",
                        foreignField: "b",
                        as: "res",
                    },
                },
                {$unwind: "$res"},
                {$addFields: {label: "found"}},
            ]).toArray();
            const after = getCounters(db);
            assert.eq(after.addFields - before.addFields, 0);
            assert.eq(after.match - before.match, 0);
            assert.eq(after.project - before.project, 0);
            assert.eq(after.replaceRoot - before.replaceRoot, 0);
        } finally {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    featureFlagSbeEqLookupUnwindLocalCollscan: true,
                }),
            );
        }
    });
});

describe("nonLeadingPushdown metrics with some disabled feature flags", function () {
    let conn, db, coll;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                internalQueryFrameworkControl: "trySbeRestricted",
                featureFlagSbeFull: false,
                featureFlagSbeNonLeadingMatch: false,
                featureFlagSbeTransformStages: false,
            },
        });
        assert.neq(null, conn);
        db = conn.getDB(jsTestName());
        coll = db.coll;
        coll.drop();
        for (let i = 0; i < 5; i++) {
            assert.commandWorked(coll.insert({_id: i, x: i % 3}));
        }
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("does not increment match", function () {
        const before = getCounters(db);
        coll.aggregate([{$group: {_id: "$x"}}, {$match: {_id: {$gt: 0}}}]).toArray();
        const after = getCounters(db);
        assert.eq(after.match - before.match, 0);
    });

    it("does not increment project", function () {
        const before = getCounters(db);
        coll.aggregate([
            {$group: {_id: "$x", total: {$sum: 1}}},
            {$project: {_id: 0, total: 1}},
        ]).toArray();
        const after = getCounters(db);
        assert.eq(after.project - before.project, 0);
    });
});
