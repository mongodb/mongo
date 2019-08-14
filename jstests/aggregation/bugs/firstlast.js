/**
 * Tests the $first and $last accumulators in $group.
 */
(function() {
'use strict';
const coll = db.jstests_aggregation_firstlast;
coll.drop();

/** Check expected $first and $last result values. */
function assertFirstLast(expectedFirst, expectedLast, stages, expression) {
    let pipeline = [{$sort: {_id: 1}}];
    if (stages) {
        pipeline = pipeline.concat(stages);
    }

    expression = expression || '$b';
    pipeline.push({$group: {_id: '$a', first: {$first: expression}, last: {$last: expression}}});

    const result = coll.aggregate(pipeline).toArray();
    for (let i = 0; i < result.length; ++i) {
        if (result[i]._id === 1) {
            // Check results for group _id 1.
            assert.eq(expectedFirst, result[i].first);
            assert.eq(expectedLast, result[i].last);
            return;
        }
    }
    throw new Error('Expected $group _id "1" is missing');
}

// One document.
assert.commandWorked(coll.insert({a: 1, b: 1}));
assertFirstLast(1, 1);

// Two documents.
assert.commandWorked(coll.insert({a: 1, b: 2}));
assertFirstLast(1, 2);

// Three documents.
assert.commandWorked(coll.insert({a: 1, b: 3}));
assertFirstLast(1, 3);

// Another 'a' key value does not affect outcome.
assert(coll.drop());
assert.commandWorked(coll.insert({a: 3, b: 0}));
assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 1, b: 3}));
assert.commandWorked(coll.insert({a: 2, b: 0}));
assertFirstLast(1, 3);

// Additional pipeline stages do not affect outcome if order is maintained.
assertFirstLast(1, 3, [{$project: {x: '$a', y: '$b'}}, {$project: {a: '$x', b: '$y'}}]);

// Additional pipeline stages affect outcome if order is modified.
assertFirstLast(3, 1, [{$sort: {b: -1}}]);

// Skip and limit affect the results seen.
assert(coll.drop());
assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 1, b: 3}));
assertFirstLast(1, 2, [{$limit: 2}]);
assertFirstLast(2, 3, [{$skip: 1}, {$limit: 2}]);
assertFirstLast(2, 2, [{$skip: 1}, {$limit: 1}]);

// Mixed type values.
assert.commandWorked(coll.insert({a: 1, b: 'foo'}));
assertFirstLast(1, 'foo');

assert(coll.drop());
assert.commandWorked(coll.insert({a: 1, b: 'bar'}));
assert.commandWorked(coll.insert({a: 1, b: true}));
assertFirstLast('bar', true);

// Value null.
assert(coll.drop());
assert.commandWorked(coll.insert({a: 1, b: null}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assertFirstLast(null, 2);

assert(coll.drop());
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 1, b: null}));
assertFirstLast(2, null);

assert(coll.drop());
assert.commandWorked(coll.insert({a: 1, b: null}));
assert.commandWorked(coll.insert({a: 1, b: null}));
assertFirstLast(null, null);

// Value missing.
assert(coll.drop());
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assertFirstLast(undefined, 2);

assert(coll.drop());
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 1}));
assertFirstLast(2, undefined);

assert(coll.drop());
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 1}));
assertFirstLast(undefined, undefined);

// Dotted field.
assert(coll.drop());
assert.commandWorked(coll.insert({a: 1, b: [{c: 1}, {c: 2}]}));
assert.commandWorked(coll.insert({a: 1, b: [{c: 6}, {}]}));
assertFirstLast([1, 2], [6], [], '$b.c');

// Computed expressions.
assert(coll.drop());
assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assertFirstLast(1, 0, [], {$mod: ['$b', 2]});
assertFirstLast(0, 1, [], {$mod: [{$add: ['$b', 1]}, 2]});
}());
