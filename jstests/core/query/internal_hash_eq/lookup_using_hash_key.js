/**
 * Tests that the combination of $meta: "indexKey", $lookup, and the $toHashedIndexKey expressions
 * can be used to look up values by their hash key.
 *
 * This is expected to work by optimizing into an $_internalEqHash match expression which can be
 * used to seek the hashed index.
 * @tags: [
 *   does_not_support_transactions,
 *   requires_fcv_70,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");         // For 'isCollscan()' and similar.
load("jstests/aggregation/extras/utils.js");  // For 'resultsEq().'

const coll = db.lookup_using_hash_key;

coll.drop();

const allDocs = [{_id: 0}, {_id: 1, a: 3}, {_id: 2, a: "3"}];
assert.commandWorked(coll.insert(allDocs));
assert.commandWorked(coll.createIndex({a: "hashed"}));

let results = coll.aggregate(
    [
        {$set: {
            hashVal: {
                $let: {
                    vars: {key: {$meta: "indexKey"}},
                    in: "$$key.a"
                }
            }
        }},
        {$lookup: {
            from: coll.getName(),
            let: {correlated_hash: "$hashVal"},
            as: "relookup",
            pipeline: [{$match: {$expr: {$eq: [{$toHashedIndexKey: "$a"}, "$$correlated_hash"]}}}]
        }},
        {$unset: "hashVal"},
    ],
    {hint: {a: "hashed"}}).toArray();

// We essentially just looked up ourselves for each document.
let expected = allDocs.map(doc => Object.merge(doc, {relookup: [doc]}));
assert(resultsEq(results, expected, true), [results, expected]);
}());
