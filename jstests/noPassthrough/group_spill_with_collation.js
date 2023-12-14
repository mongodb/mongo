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

MongoRunner.stopMongod(conn);
