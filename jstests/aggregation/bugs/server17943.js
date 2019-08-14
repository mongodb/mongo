// SERVER-17943: Add $filter aggregation expression.

// For assertErrorCode.
load('jstests/aggregation/extras/utils.js');

(function() {
'use strict';

var coll = db.agg_filter_expr;
coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: [1, 2, 3, 4, 5]}));
assert.commandWorked(coll.insert({_id: 1, a: [2, 4]}));
assert.commandWorked(coll.insert({_id: 2, a: []}));
assert.commandWorked(coll.insert({_id: 3, a: [1]}));
assert.commandWorked(coll.insert({_id: 4, a: null}));
assert.commandWorked(coll.insert({_id: 5, a: undefined}));
assert.commandWorked(coll.insert({_id: 6}));

// Create filter to only accept odd numbers.
filterDoc = {input: '$a', as: 'x', cond: {$eq: [1, {$mod: ['$$x', 2]}]}};
var expectedResults = [
    {_id: 0, b: [1, 3, 5]},
    {_id: 1, b: []},
    {_id: 2, b: []},
    {_id: 3, b: [1]},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];
var results = coll.aggregate([{$project: {b: {$filter: filterDoc}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, expectedResults);

// create filter that uses the default variable name in 'cond'
filterDoc = {
    input: '$a',
    cond: {$eq: [2, '$$this']}
};
expectedResults = [
    {_id: 0, b: [2]},
    {_id: 1, b: [2]},
    {_id: 2, b: []},
    {_id: 3, b: []},
    {_id: 4, b: null},
    {_id: 5, b: null},
    {_id: 6, b: null},
];
results = coll.aggregate([{$project: {b: {$filter: filterDoc}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, expectedResults);

// Invalid filter expressions.

// '$filter' is not a document.
var filterDoc = 'string';
assertErrorCode(coll, [{$project: {b: {$filter: filterDoc}}}], 28646);

// Extra field(s).
filterDoc = {input: '$a', as: 'x', cond: true, extra: 1};
assertErrorCode(coll, [{$project: {b: {$filter: filterDoc}}}], 28647);

// Missing 'input'.
filterDoc = {
    as: 'x',
    cond: true
};
assertErrorCode(coll, [{$project: {b: {$filter: filterDoc}}}], 28648);

// Missing 'cond'.
filterDoc = {input: '$a', as: 'x'};
assertErrorCode(coll, [{$project: {b: {$filter: filterDoc}}}], 28650);

// 'as' is not a valid variable name.
filterDoc = {input: '$a', as: '$x', cond: true};
assertErrorCode(coll, [{$project: {b: {$filter: filterDoc}}}], 16867);

// 'input' is not an array.
filterDoc = {input: 'string', as: 'x', cond: true};
assertErrorCode(coll, [{$project: {b: {$filter: filterDoc}}}], 28651);

// 'cond' uses undefined variable name.
filterDoc = {
    input: '$a',
    cond: {$eq: [1, '$$var']}
};
assertErrorCode(coll, [{$project: {b: {$filter: filterDoc}}}], 17276);

assert(coll.drop());
assert.commandWorked(coll.insert({a: 'string'}));
filterDoc = {input: '$a', as: 'x', cond: true};
assertErrorCode(coll, [{$project: {b: {$filter: filterDoc}}}], 28651);
}());
