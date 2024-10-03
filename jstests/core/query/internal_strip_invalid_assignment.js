/**
 * Test that '$_internalSchemaAllElemMatchFromIndex' does not use an index, and does not trigger an
 * assertion failure when we strip its index assignment from special indexes.
 *
 * Originally intended to reproduce SERVER-82717.
 *
 * @tags: [
 *    # Explain command does not support read concerns other than local.
 *    assumes_read_concern_local,
 * ]
 */
import {planHasStage} from 'jstests/libs/query/analyze_plan.js';

const coll = db.getCollection(jsTestName());

assert.commandWorked(coll.createIndex({a: 1, foo: '2dsphere'}));
assert.commandWorked(coll.createIndex({a: 1, '$**': 'text'}));
assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {foo: 123}}));

const query = {
    "$or": [
        // Has to be the same field.
        {"a": 1},
        {
            "a": {
                // Checks that ALL elements of an array match this predicate.
                // An index on {a: 1, ...} doesn't help, because:
                // - If it's multikey, each index entry only tells us one array-element, while
                //   the predicate needs to check all elements.
                // - If it's non-multikey, the predicate is trivially false, because it expects
                //   an array.
                "$_internalSchemaAllElemMatchFromIndex": [
                    // Number of elements to skip.
                    NumberLong(0),
                    // Predicate to run on each element.
                    {"i": {"$regex": "b"}},
                ]
            }
        }
    ]
};
const explain = coll.find(query).explain();
assert(planHasStage(db, explain, "COLLSCAN"), explain);
