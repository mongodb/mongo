/**
 * Some more tests $and/$or being nested in various ways.
 */
import {arrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db.jstests_and_or_nested;
coll.drop();

function runWithDifferentIndexes(keyPatternsList, testFunc) {
    for (const keyPatterns of keyPatternsList) {
        for (const keyPattern of keyPatterns) {
            assert.commandWorked(coll.createIndex(keyPattern));
        }
        testFunc();
        assert.commandWorked(coll.dropIndexes());
    }
}

assert.commandWorked(coll.insert([
    {_id: 1, a: 8, b: 3, c: 4, d: 0},
    {_id: 2, a: 1, b: 5, c: 9, d: 1},
    {_id: 3, a: 6, b: 7, c: 2, d: 1},
    {_id: 4, a: 4, b: 8, c: 3, d: 0},
    {_id: 5, a: 9, b: 1, c: 5, d: 1},
    {_id: 6, a: 2, b: 6, c: 7, d: 0},
    {_id: 7, a: 3, b: 4, c: 8, d: 0},
    {_id: 8, a: 5, b: 9, c: 1, d: 0},
    {_id: 9, a: 7, b: 2, c: 6, d: 1},
    {_id: 10, b: 3, c: 4, d: 0},
    {_id: 11, a: 8, b: 3, d: 0},
    {_id: 12, a: 9, c: 5, d: 1},
    {_id: 13, a: 9, b: 1, d: 1}
]));

runWithDifferentIndexes([[], [{a: 1}], [{b: 1}], [{a: 1}, {b: 1}], [{a: 1}, {c: 1}]], () => {
    assert(arrayEq(coll.find({$and: [{a: {$gt: 5}}, {c: {$gt: 4}}]}, {_id: 1}).toArray(),
                   [{_id: 5}, {_id: 9}, {_id: 12}]));
    assert(
        arrayEq(coll.find({$and: [{$or: [{a: {$gt: 5}}, {b: {$lt: 5}}]}, {c: {$lt: 6}}]}, {_id: 1})
                    .toArray(),
                [{_id: 1}, {_id: 3}, {_id: 5}, {_id: 10}, {_id: 12}]));

    assert(arrayEq(coll.find({$or: [{b: {$gte: 7}}]}, {_id: 1}).toArray(),
                   [{_id: 3}, {_id: 4}, {_id: 8}]));

    assert(arrayEq(coll.find({$or: [{$and: [{b: {$gte: 7}}]}]}, {_id: 1}).toArray(),
                   [{_id: 3}, {_id: 4}, {_id: 8}]));

    assert(arrayEq(coll.find({$or: [{a: {$gt: 6}}, {b: {$lt: 4}}]}, {_id: 1}).toArray(),
                   [{_id: 1}, {_id: 5}, {_id: 9}, {_id: 10}, {_id: 11}, {_id: 12}, {_id: 13}]));

    assert(arrayEq(
        coll.find({$or: [{$and: [{a: {$gt: 5}}, {c: {$gt: 2}}]}, {$and: [{b: {$lt: 5}}, {d: 1}]}]},
                  {_id: 1})
            .toArray(),
        [{_id: 1}, {_id: 5}, {_id: 9}, {_id: 12}, {_id: 13}]));

    assert(arrayEq(
        coll.find({$or: [{a: {$gt: 8}}, {$and: [{b: {$lt: 5}}, {$or: [{c: {$lt: 5}}, {d: 1}]}]}]},
                  {_id: 1})
            .toArray(),
        [{_id: 1}, {_id: 5}, {_id: 9}, {_id: 10}, {_id: 12}, {_id: 13}]));
});
