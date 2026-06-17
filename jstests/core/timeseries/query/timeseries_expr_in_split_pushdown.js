/**
 * Regression test for SERVER-124974: a bug where $expr with $and containing $in predicates might
 * contain dangling pointers when the predicate is split during $match pushdown analysis.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}),
);

const t0 = ISODate("2024-01-01T00:00:00Z");
const t1 = ISODate("2024-01-02T00:00:00Z");
assert.commandWorked(
    coll.insertMany([
        {t: t0, m: "tagA", x: 1, y: 10},
        {t: t0, m: "tagA", x: 2, y: 20},
        {t: t1, m: "tagA", x: 3, y: 10},
        {t: t0, m: "tagB", x: 1, y: 10},
    ]),
);

// $expr with predicates only on measurement columns.
assert.sameMembers(
    coll
        .aggregate([
            {$match: {m: "tagA", $expr: {$and: [{$in: ["$x", [1, 2]]}, {$eq: ["$y", 10]}]}}},
            {$project: {_id: 0, m: 1, x: 1, y: 1}},
        ])
        .toArray(),
    [{m: "tagA", x: 1, y: 10}],
);

// $expr with predicates on both the time column and measurement columns.
assert.sameMembers(
    coll
        .aggregate([
            {$match: {m: "tagA", $expr: {$and: [{$in: ["$t", [t0]]}, {$in: ["$x", [1, 2]]}]}}},
            {$project: {_id: 0, x: 1}},
            {$sort: {x: 1}},
        ])
        .toArray(),
    [{x: 1}, {x: 2}],
);

// $expr with predicates on both the meta column (independent, pushed before unpack) and a
// measurement column (dependent, stays after unpack), forcing the split to produce both an
// independent and dependent part.
assert.sameMembers(
    coll
        .aggregate([
            {$match: {$expr: {$and: [{$in: ["$m", ["tagA", "tagB"]]}, {$in: ["$x", [1, 3]]}]}}},
            {$project: {_id: 0, m: 1, x: 1, y: 1}},
        ])
        .toArray(),
    [
        {m: "tagA", x: 1, y: 10},
        {m: "tagA", x: 3, y: 10},
        {m: "tagB", x: 1, y: 10},
    ],
);

// $expr with $in predicates on two measurement columns.
assert.sameMembers(
    coll
        .aggregate([
            {$match: {$expr: {$and: [{$in: ["$x", [1, 2]]}, {$in: ["$y", [10, 20]]}]}}},
            {$project: {_id: 0, m: 1, x: 1, y: 1}},
        ])
        .toArray(),
    [
        {m: "tagA", x: 1, y: 10},
        {m: "tagA", x: 2, y: 20},
        {m: "tagB", x: 1, y: 10},
    ],
);
