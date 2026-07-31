/**
 * Regression test checking that a $match on an ancestor of a rename onto a dotted path is not
 * erroneously pushed in front of the $set which performs the rename.
 *
 * Renaming onto "a.b.c" materializes the missing ancestors "a" and "a.b" as empty objects, so a
 * predicate on an ancestor is not independent of the $set and the two stages cannot be swapped.
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

// Each $match predicate depends on an ancestor of this path, so will only match once the $set has
// materialized that ancestor. The value being renamed has no bearing on the result.
const setStage = {$set: {"a.b.c": "$x"}};
const expected = [{_id: 0, x: 1, a: {b: {c: 1}}}];

describe('$match on an ancestor of the rename onto "a.b.c"', function () {
    let coll;

    before(function () {
        coll = db[jsTestName()];
        coll.drop();
        assert.commandWorked(coll.insert({_id: 0, x: 1}));
        // The presence of a non-multikey index lets the dependency analysis conclude that no arrays
        // are involved.
        assert.commandWorked(coll.createIndex({"a.b.c.d": 1}));
    });

    it("is not pushed before the $set when the $match uses an $expr with $type", function () {
        let pipeline = [setStage, {$match: {$expr: {$eq: [{$type: "$a.b"}, "object"]}}}];
        assert.eq(coll.aggregate(pipeline).toArray(), expected);

        pipeline = [setStage, {$match: {$expr: {$eq: [{$type: "$a"}, "object"]}}}];
        assert.eq(coll.aggregate(pipeline).toArray(), expected);
    });

    it("is not pushed before the $set when the $match uses an $expr with $setField", function () {
        let pipeline = [
            setStage,
            {$match: {$expr: {$setField: {field: "_id", input: "$a.b", value: 1}}}},
        ];
        assert.eq(coll.aggregate(pipeline).toArray(), expected);

        pipeline = [
            setStage,
            {$match: {$expr: {$setField: {field: "_id", input: "$a", value: 1}}}},
        ];
        assert.eq(coll.aggregate(pipeline).toArray(), expected);
    });

    it("is not pushed before the $set when the $match uses a plain predicate", function () {
        let pipeline = [setStage, {$match: {"a.b": {$type: "object"}}}];
        assert.eq(coll.aggregate(pipeline).toArray(), expected);

        pipeline = [setStage, {$match: {a: {$type: "object"}}}];
        assert.eq(coll.aggregate(pipeline).toArray(), expected);
    });
});
