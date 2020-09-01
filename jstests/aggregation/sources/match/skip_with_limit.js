/**
 * Tests that the server returns the correct number of results in the presence of multiple $skips
 * and $limits. There are a number of optimizations and special checks the server implements -
 * especially in a sharded cluster - which we intend to stress with this test.
 */
(function() {
"use strict";

const coll = db.skip_with_limit;
coll.drop();

// Insert twenty documents: {x: 4, y: 0}, {x: 4, y: 1}, ..., {x: 4, y: 19}.
const bulk = coll.initializeOrderedBulkOp();
Array.from({length: 20}, (_, i) => ({x: 4, y: i})).forEach(doc => bulk.insert(doc));
assert.commandWorked(bulk.execute());

// Test pipelines with $skip before $limit.
var count = coll.aggregate([{$match: {x: 4}}, {$skip: 10}, {$limit: 5}]).itcount();
assert.eq(count, 5);

count = coll.aggregate([{$match: {x: 4}}, {$skip: 7}, {$skip: 3}, {$limit: 5}]).itcount();
assert.eq(count, 5);

count = coll.aggregate([{$match: {x: 4}}, {$skip: 10}, {$limit: 5}]).itcount();
assert.eq(count, 5);

count =
    coll.aggregate([{$match: {x: 4}}, {$skip: 10}, {$addFields: {y: 1}}, {$limit: 5}]).itcount();
assert.eq(count, 5);

count =
    coll.aggregate([{$match: {x: 4}}, {$skip: 10}, {$group: {_id: '$y'}}, {$limit: 5}]).itcount();
assert.eq(count, 5);

// Test pipelines with $limit before $skip.
count = coll.aggregate([{$match: {x: 4}}, {$limit: 10}, {$skip: 5}]).itcount();
assert.eq(count, 5);

count = coll.aggregate([{$match: {x: 4}}, {$limit: 7}, {$limit: 3}, {$skip: 1}]).itcount();
assert.eq(count, 2);

count = coll.aggregate([{$match: {x: 4}}, {$limit: 10}, {$skip: 5}]).itcount();
assert.eq(count, 5);

count =
    coll.aggregate([{$match: {x: 4}}, {$limit: 10}, {$addFields: {y: 1}}, {$skip: 5}]).itcount();
assert.eq(count, 5);

count =
    coll.aggregate([{$match: {x: 4}}, {$limit: 10}, {$group: {_id: '$y'}}, {$skip: 5}]).itcount();
assert.eq(count, 5);

// For the pipelines with a $skip before the $limit, repeat the tests with larger skip values to
// ensure that the skip is actually working. The large skips exhaust our 20 documents, so we get
// fewer results.
count = coll.aggregate([{$match: {x: 4}}, {$skip: 18}, {$limit: 5}]).itcount();
assert.eq(count, 2);

count = coll.aggregate([{$match: {x: 4}}, {$skip: 11}, {$skip: 7}, {$limit: 5}]).itcount();
assert.eq(count, 2);

count =
    coll.aggregate([{$match: {x: 4}}, {$skip: 18}, {$addFields: {y: 1}}, {$limit: 5}]).itcount();
assert.eq(count, 2);

count =
    coll.aggregate([{$match: {x: 4}}, {$skip: 18}, {$group: {_id: '$y'}}, {$limit: 5}]).itcount();
assert.eq(count, 2);

// Now add some pipelines that have multiple consecutive skips to test that our logic to swap a
// limit in front of a skip adds the correct total to the limit. For example, in the first test the
// limit should end up being 23. Here we also throw in some tests with $sort stages, because $sort
// stages will try to pull limits forward.
count = coll.aggregate([{$match: {x: 4}}, {$sort: {x: 1}}, {$skip: 10}, {$skip: 8}, {$limit: 5}])
            .itcount();
assert.eq(count, 2);

count =
    coll.aggregate([{$match: {x: 4}}, {$skip: 5}, {$limit: 10}, {$skip: 5}, {$limit: 4}]).itcount();
assert.eq(count, 4);

count = coll.aggregate([{$match: {x: 4}}, {$skip: 7}, {$skip: 4}, {$limit: 4}]).itcount();
assert.eq(count, 4);
count = coll.aggregate([{$match: {x: 4}}, {$sort: {y: -1}}, {$skip: 7}, {$skip: 4}, {$limit: 4}])
            .itcount();
assert.eq(count, 4);
count = coll.aggregate([{$match: {x: 4}}, {$skip: 7}, {$skip: 10}, {$limit: 4}]).itcount();
assert.eq(count, 3);
count = coll.aggregate([{$match: {x: 4}}, {$sort: {y: -1}}, {$skip: 7}, {$skip: 10}, {$limit: 4}])
            .itcount();
assert.eq(count, 3);

// Prevent $sort stage from being pushdowned to the find layer and check that the code folding
// $limit stages in this case respects values from $skip stages.
count = coll.aggregate([
                {$match: {x: 4}},
                {$_internalInhibitOptimization: {}},
                {$sort: {x: 1}},
                {$skip: 10},
                {$skip: 8},
                {$limit: 5}
            ])
            .itcount();
assert.eq(count, 2);

count = coll.aggregate([
                {$match: {x: 4}},
                {$_internalInhibitOptimization: {}},
                {$sort: {y: -1}},
                {$skip: 7},
                {$skip: 4},
                {$limit: 4}
            ])
            .itcount();
assert.eq(count, 4);

count = coll.aggregate([
                {$match: {x: 4}},
                {$_internalInhibitOptimization: {}},
                {$sort: {y: -1}},
                {$skip: 7},
                {$skip: 10},
                {$limit: 4}
            ])
            .itcount();
assert.eq(count, 3);
}());
