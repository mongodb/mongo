/**
 * Test $match with $exists is supported and returns correct results.
 */

(function() {
"use strict";

const coll = db.cqf_golden_match_with_exists;

const runTest = (filter) => {
    const pipeline = [{$match: filter}];
    jsTestLog(`Query: ${tojsononeline(pipeline)}`);
    show(coll.aggregate(pipeline));
};

const runWithData = (docs, filters) => {
    coll.drop();
    jsTestLog("Inserting docs:");
    show(docs);
    assert.commandWorked(coll.insert(docs));
    print(`Collection count: ${coll.count()}`);
    for (const filter of filters) {
        runTest(filter);
    }
};

runWithData(
    [
        {_id: 0},
        {_id: 1, a: null},
        {_id: 2, a: 1},
        {_id: 3, b: null},
        {_id: 4, b: 2},
        {_id: 5, 'a': {'b': 3}},
        {_id: 6, 'a': [{'b': 4}]}
    ],
    [
        {a: {$exists: true}},
        {a: {$not: {$exists: false}}},
        {a: {$exists: false}},
        {b: {$exists: true}},
        {'a.b': {$exists: true}},
        {'a.b': {$exists: false}},
    ]);

runWithData(
    [
        {_id: 1, a: []},
    ],
    [{'a': {$exists: true}}, {'a': {$exists: false}}]);

runWithData(
    [
        {_id: 1, a: false},
    ],
    [{'a': {$exists: true}}, {'a': {$exists: false}}]);

runWithData([{_id: 1, a: [{'b': 2}, {'a': 1}]}],
            [{'a.a': {$exists: true}}, {'a.a': {$exists: false}}, {'a.b': {$exists: true}}]);

runWithData([{_id: 1, a: [[{b: 1}]]}], [{'a.b': {$exists: false}}, {'a.b': {$exists: true}}]);

runWithData(
    [
        {_id: 1, a: [1]},
        {_id: 2, a: [2]},
    ],
    [{'a': {$elemMatch: {$exists: true}}}, {'a': {$elemMatch: {$exists: false}}}]);
})();
