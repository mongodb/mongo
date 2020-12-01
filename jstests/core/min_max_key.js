// Tests the behavior of queries using MinKey and MaxKey

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For 'resultsEq'.

const coll = db.test_min_max;
coll.drop();

const allElements = [
    {_id: "a_max_key", a: MaxKey},
    {_id: "a_min_key", a: MinKey},
    {_id: "a_null", a: null},
    {_id: "a_number", a: 4},
    {_id: "a_subobject", a: {b: "hi"}},
    {_id: "a_undefined", a: undefined},
    {_id: "a_string", a: "hello"}
];

assert.commandWorked(coll.insert(allElements));

function testQueriesWithMinOrMaxKey() {
    const eqMinRes = coll.find({a: {$eq: MinKey}}).toArray();
    const expectedEqMin = [{_id: "a_min_key", a: MinKey}];
    assert(resultsEq(expectedEqMin, eqMinRes), tojson(eqMinRes));

    const gtMinRes = coll.find({a: {$gt: MinKey}}).toArray();
    const expectedGtMin = [
        {_id: "a_max_key", a: MaxKey},
        {_id: "a_null", a: null},
        {_id: "a_number", a: 4},
        {_id: "a_subobject", a: {b: "hi"}},
        {_id: "a_undefined", a: undefined},
        {_id: "a_string", a: "hello"}
    ];
    assert(resultsEq(expectedGtMin, gtMinRes), tojson(gtMinRes));

    const gteMinRes = coll.find({a: {$gte: MinKey}}).toArray();
    assert(resultsEq(allElements, gteMinRes), tojson(gteMinRes));

    const ltMinRes = coll.find({a: {$lt: MinKey}}).toArray();
    assert(resultsEq([], ltMinRes), tojson(ltMinRes));

    const lteMinRes = coll.find({a: {$lte: MinKey}}).toArray();
    assert(resultsEq(expectedEqMin, lteMinRes), tojson(lteMinRes));

    const eqMaxRes = coll.find({a: {$eq: MaxKey}}).toArray();
    const expectedEqMax = [{_id: "a_max_key", a: MaxKey}];
    assert(resultsEq(expectedEqMax, eqMaxRes), tojson(eqMaxRes));

    const gtMaxRes = coll.find({a: {$gt: MaxKey}}).toArray();
    assert(resultsEq([], gtMaxRes), tojson(gtMaxRes));

    const gteMaxRes = coll.find({a: {$gte: MaxKey}}).toArray();
    assert(resultsEq(expectedEqMax, gteMaxRes), tojson(gteMaxRes));

    const ltMaxRes = coll.find({a: {$lt: MaxKey}}).toArray();
    const expectedLtMax = [
        {_id: "a_min_key", a: MinKey},
        {_id: "a_null", a: null},
        {_id: "a_number", a: 4},
        {_id: "a_subobject", a: {b: "hi"}},
        {_id: "a_undefined", a: undefined},
        {_id: "a_string", a: "hello"}
    ];
    assert(resultsEq(expectedLtMax, ltMaxRes), tojson(ltMaxRes));

    const lteMaxRes = coll.find({a: {$lte: MaxKey}}).toArray();
    assert(resultsEq(allElements, lteMaxRes), tojson(lteMaxRes));
}

function testTypeBracketedQueries() {
    // Queries that do not involve MinKey or MaxKey follow type bracketing and thus do not
    // return MinKey or MaxKey as results. These queries are being run to test this
    // functionality.
    const numRes = coll.find({a: {$gt: 3}}).toArray();
    const expectedNum = [{_id: "a_number", a: 4}];
    assert(resultsEq(expectedNum, numRes), tojson(numRes));
    const noNum = coll.find({a: {$lt: 3}}).toArray();
    assert(resultsEq([], noNum), tojson(noNum));

    const stringRes = coll.find({a: {$gt: "best"}}).toArray();
    const expectedString = [{_id: "a_string", a: "hello"}];
    assert(resultsEq(expectedString, stringRes), tojson(stringRes));
}

testQueriesWithMinOrMaxKey();
testTypeBracketedQueries();

assert.commandWorked(coll.createIndex({a: 1}));

testQueriesWithMinOrMaxKey();
testTypeBracketedQueries();
}());
