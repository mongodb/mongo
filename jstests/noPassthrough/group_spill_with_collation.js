/**
 * Tests $group execution with increased spilling and a non-simple collation.
 *
 * @tags: [featureFlagSbeFull]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB('test');
const coll = db.group_pushdown_with_collation;
coll.drop();
for (let i = 0; i < 1000; i++) {
    if (i % 3 === 0) {
        assert.commandWorked(coll.insert({x: 'a'}));
    } else {
        assert.commandWorked(coll.insert({x: 'A'}));
    }
}

// Test that accumulators respect the collation when the group operation spills to disk.
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling: true}));
const caseInsensitive = {
    collation: {locale: "en_US", strength: 2}
};
let results =
    coll.aggregate([{$group: {_id: null, result: {$addToSet: "$x"}}}], caseInsensitive).toArray();
assert.eq(1, results.length, results);
assert.eq({_id: null, result: ["a"]}, results[0]);

// Test that comparisons of the group key respect the collation when the group operation spills to
// disk.
coll.drop();
for (let i = 0; i < 1000; i++) {
    if (i % 5 === 0) {
        if (i % 10 === 0) {
            assert.commandWorked(coll.insert({x: 'b', y: 'D'}));
        } else {
            assert.commandWorked(coll.insert({x: 'b', y: 'e'}));
        }
    } else {
        assert.commandWorked(coll.insert({x: 'B', y: 'd'}));
    }
}

results =
    coll.aggregate(
            [{
                $group:
                    {_id: {X: "$x", Y: "$y"}, val: {$first: {$toLower: "$y"}}, count: {$count: {}}}
            }],
            caseInsensitive)
        .toArray();
assertArrayEq({
    actual: results,
    expected: [{val: "d", count: 900}, {val: "e", count: 100}],
    fieldsToSkip: ["_id"]
});

// Re-issue the query with the simple collation and check that the grouping becomes case-sensitive.
results =
    coll.aggregate([{
            $group: {_id: {X: "$x", Y: "$y"}, val: {$first: {$toLower: "$y"}}, count: {$count: {}}}
        }])
        .toArray();
assertArrayEq({
    actual: results,
    expected: [{val: "d", count: 800}, {val: "d", count: 100}, {val: "e", count: 100}],
    fieldsToSkip: ["_id"]
});

// Test that comparisons of the group key respect the collation when the group operation spills to
// disk and the key is an array.
coll.drop();
assert.commandWorked(coll.insertMany([
    {"_id": 0, key: ["A", "b"]},
    {"_id": 1, key: ["A", "B"]},
    {"_id": 2, key: ["B"]},
    {"_id": 3, key: ["b"]},
    {"_id": 4, key: ["a", "B"]},
    {"_id": 5, key: ["a", "b"]},
]));

results = coll.aggregate([{$group: {_id: "$key", count: {$count: {}}}}], caseInsensitive).toArray();
assertArrayEq({
    actual: results,
    expected: [{_id: ["A", "b"], count: 4}, {_id: ["B"], count: 2}],
});

// Re-issue the query with the simple collation and check that the grouping becomes case-sensitive.
results = coll.aggregate([{$group: {_id: "$key", count: {$count: {}}}}]).toArray();

assertArrayEq({
    actual: results,
    expected: [
        {_id: ["A", "b"], count: 1},
        {_id: ["A", "B"], count: 1},
        {_id: ["B"], count: 1},
        {_id: ["b"], count: 1},
        {_id: ["a", "B"], count: 1},
        {_id: ["a", "b"], count: 1}
    ],
});

// Test that comparisons of the group key respect the collation when the group operation spills to
// disk and the key is an object or an array.
coll.drop();
for (let i = 0; i < 1000; i++) {
    if (i % 5 === 0) {
        assert.commandWorked(coll.insert({f: {val: 'A'}}));
    } else {
        assert.commandWorked(coll.insert({f: {val: 'a'}}));
    }
}

for (let i = 0; i < 1000; i++) {
    if (i % 5 === 0) {
        assert.commandWorked(coll.insert({f: [{"x": 1, "y": 2}]}));
    } else {
        assert.commandWorked(coll.insert({f: {"w": {"x": 1, "y": 2}}}));
    }
}

results = coll.aggregate([{$group: {_id: {F: "$f"}, firstF: {$first: "$f"}, count: {$count: {}}}}],
                         caseInsensitive)
              .toArray();
assertArrayEq({
    actual: results,
    expected: [
        {firstF: [{"x": 1, "y": 2}], count: 200},
        {firstF: {"w": {"x": 1, "y": 2}}, count: 800},
        {firstF: {val: "A"}, count: 1000}
    ],
    fieldsToSkip: ["_id"]
});

// Re-issue the query with the simple collation and check that the grouping becomes case-sensitive.
results = coll.aggregate([{$group: {_id: {F: "$f"}, firstF: {$first: "$f"}, count: {$count: {}}}}])
              .toArray();
assertArrayEq({
    actual: results,
    expected: [
        {firstF: [{"x": 1, "y": 2}], count: 200},
        {firstF: {"w": {"x": 1, "y": 2}}, count: 800},
        {firstF: {val: "a"}, count: 800},
        {firstF: {val: "A"}, count: 200}
    ],
    fieldsToSkip: ["_id"]
});

MongoRunner.stopMongod(conn);
