/**
 * Tests that queries using a multikey $** index, return correct results.
 * @tags: [assumes_balancer_off]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.
load("jstests/libs/analyze_plan.js");         // For getPlanStages.

const assertArrayEq = (l, r) => assert(arrayEq(l, r), tojson(l) + " != " + tojson(r));

const coll = db.wildcard_multikey_index;
coll.drop();

// Template document which defines the 'schema' of the documents in the test collection.
const templateDoc = {
    a: [],
    b: {c: [], d: [{e: 0}]}
};
const pathList = ["a", "b.c", "b.d.e"];

// Insert a set of documents into the collection, based on the template document and populated
// with an increasing sequence of values. This is to ensure that the range of values present for
// each field in the dataset is not entirely homogeneous.
for (let i = 0; i < 50; i++) {
    (function populateDoc(doc, value) {
        for (let key in doc) {
            if (typeof doc[key] === "object") {
                if (Array.isArray(doc[key])) {
                    if (typeof doc[key][0] === "object") {
                        value = populateDoc(doc[key][0], value);
                    } else {
                        doc[key] = [++value, ++value];
                    }
                } else {
                    value = populateDoc(doc[key], value);
                }
            } else {
                doc[key] = ++value;
            }
        }
        return value;
    })(templateDoc, i);
    assert.commandWorked(coll.insert(templateDoc));
}

// Set of operations which will be applied to each field in the index in turn.
const operationList = [
    {expression: {$gte: 10}},
    {expression: {$gt: 10}},
    {expression: {$lt: 40}},
    {expression: {$lte: 40}},
    {expression: {$gt: 10, $lt: 40}},
    {expression: {$eq: 25}},
    {expression: {$in: [5, 15, 35, 40]}},
    {expression: {$elemMatch: {$gte: 10, $lte: 40}}},
];

// Given a keyPattern and (optional) pathProjection, this function builds a $** index on the
// collection and then tests each of the match expression in the 'operationList' on each indexed
// field in turn. The 'expectedPaths' argument lists the set of paths which we expect to have
// been indexed based on the spec; this function will confirm that only the appropriate paths
// are present in the $** index.
function runWildcardIndexTest(keyPattern, pathProjection, expectedPaths) {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(
        coll.createIndex(keyPattern, pathProjection ? {wildcardProjection: pathProjection} : {}));
    assert(expectedPaths);
    // Verify the expected behaviour for every combination of path and operator.
    for (let op of operationList) {
        for (let path of pathList) {
            const query = {[path]: op.expression};
            assertWildcardQuery(query, expectedPaths.includes(path) ? path : null);
        }
    }
}

// Runs a single wildcard query test. If 'expectedPath' is non-null, verifies that there is an
// indexed solution that uses the $** index with the given path string. If 'expectedPath' is
// null, verifies that no indexed solution was found. If 'explainStats' is non-empty, verifies
// that the query's explain output reflects the given stats.
function assertWildcardQuery(query, expectedPath, explainStats = {}) {
    // Explain the query, and determine whether an indexed solution is available.
    const explainOutput = coll.find(query).explain("executionStats");

    // Verify that the explain output reflects the given 'explainStats'.
    for (let stat in explainStats) {
        assert.eq(explainStats[stat],
                  stat.split('.').reduce((obj, i) => obj[i], explainOutput),
                  explainOutput);
    }

    // If we expect the current path to have been excluded based on the $** keyPattern
    // or projection, confirm that no indexed solution was found.
    if (!expectedPath) {
        assert.gt(getPlanStages(explainOutput.queryPlanner.winningPlan, "COLLSCAN").length, 0);
        return;
    }
    // Verify that the winning plan uses the $** index with the expected path.
    const ixScans = getPlanStages(explainOutput.queryPlanner.winningPlan, "IXSCAN");
    assert.eq(ixScans.length, FixtureHelpers.numberOfShardsForCollection(coll));
    assert.docEq(ixScans[0].keyPattern, {"$_path": 1, [expectedPath]: 1});
    // Verify that the results obtained from the $** index are identical to a COLLSCAN.
    assertArrayEq(coll.find(query).toArray(), coll.find(query).hint({$natural: 1}).toArray());
}

// Test a $** index that indexes the entire document.
runWildcardIndexTest({'$**': 1}, null, ['a', 'b.c', 'b.d.e']);
// Test a $** index on a single subtree.
runWildcardIndexTest({'a.$**': 1}, null, ['a']);
runWildcardIndexTest({'b.$**': 1}, null, ['b.c', 'b.d.e']);
runWildcardIndexTest({'b.c.$**': 1}, null, ['b.c']);
runWildcardIndexTest({'b.d.$**': 1}, null, ['b.d.e']);
// Test a $** index which includes a subset of paths.
runWildcardIndexTest({'$**': 1}, {a: 1}, ['a']);
runWildcardIndexTest({'$**': 1}, {b: 1}, ['b.c', 'b.d.e']);
runWildcardIndexTest({'$**': 1}, {'b.d': 1}, ['b.d.e']);
runWildcardIndexTest({'$**': 1}, {a: 1, 'b.d': 1}, ['a', 'b.d.e']);
// Test a $** index which excludes a subset of paths.
runWildcardIndexTest({'$**': 1}, {a: 0}, ['b.c', 'b.d.e']);
runWildcardIndexTest({'$**': 1}, {b: 0}, ['a']);
runWildcardIndexTest({'$**': 1}, {'b.c': 0}, ['a', 'b.d.e']);
runWildcardIndexTest({'$**': 1}, {a: 0, 'b.c': 0}, ['b.d.e']);

// Sanity check that a few queries which need to be planned specially in the multikey case
// return the correct results.
coll.drop();
assert.commandWorked(coll.createIndex({"$**": 1}));
assert.commandWorked(coll.insert({a: [-5, 15]}));
assert.eq(1, coll.find({a: {$gt: 0, $lt: 9}}).itcount());
assert.eq(1, coll.find({a: {$gt: 0, $lt: 9}}).hint({$natural: 1}).itcount());
assert.eq(0, coll.find({a: {$elemMatch: {$gt: 0, $lt: 9}}}).itcount());
assert.eq(0, coll.find({a: {$elemMatch: {$gt: 0, $lt: 9}}}).hint({$natural: 1}).itcount());

assert.commandWorked(coll.insert({b: {c: {d: [{e: {f: -5}}, {e: {f: 15}}]}}}));
assert.eq(1, coll.find({"b.c.d.e.f": {$gt: 0, $lt: 9}}).itcount());
assert.eq(1, coll.find({"b.c.d.e.f": {$gt: 0, $lt: 9}}).hint({$natural: 1}).itcount());
assert.eq(0, coll.find({"b.c.d": {$elemMatch: {"e.f": {$gt: 0, $lt: 9}}}}).itcount());
assert.eq(
    0, coll.find({"b.c.d": {$elemMatch: {"e.f": {$gt: 0, $lt: 9}}}}).hint({$natural: 1}).itcount());

// Fieldname-or-array-index query tests.
assert(coll.drop());
assert.commandWorked(coll.createIndex({"$**": 1}));

// Insert some documents that exhibit a mix of numeric fieldnames and array indices.
assert.commandWorked(coll.insert({_id: 1, a: [{b: [{c: 1}]}]}));
assert.commandWorked(coll.insert({_id: 2, a: [{b: [{c: 0}, {c: 1}]}]}));
assert.commandWorked(coll.insert({_id: 3, a: {'0': [{b: {'1': {c: 1}}}, {d: 1}]}}));
assert.commandWorked(coll.insert({_id: 4, a: [{b: [{1: {c: 1}}]}]}));
assert.commandWorked(
    coll.insert({_id: 5, a: [{b: [{'1': {c: {'2': {d: [0, 1, 2, 3, {e: 1}]}}}}]}]}));

/*
 * Multikey Metadata Keys:
 * {'': 1, '': 'a'}
 * {'': 1, '': 'a.0'}
 * {'': 1, '': 'a.b'}
 * {'': 1, '': 'a.b.1.c.2.d'}
 * Keys:
 * {'': 'a.b.c', '': 1}         // _id: 1, a,b multikey
 * {'': 'a.b.c', '': 0}         // _id: 2, a,b multikey
 * {'': 'a.b.c', '': 1}         // _id: 2, a,b multikey
 * {'': 'a.0.b.1.c', '': 1}     // _id: 3, '0, 1' are fieldnames, a.0 multikey
 * {'': 'a.0.d', '': 1}         // _id: 3, '0' is fieldname, a.0 multikey
 * {'': 'a.b.1.c', '': 1}       // _id: 4, '1' is fieldname, a,b multikey
 * {'': 'a.b.1.c.2.d', '': 0}   // _id: 5, a,b,a.b.1.c.2.d multikey, '1' is fieldname
 * {'': 'a.b.1.c.2.d', '': 1}   // _id: 5
 * {'': 'a.b.1.c.2.d', '': 2}   // _id: 5
 * {'': 'a.b.1.c.2.d', '': 3}   // _id: 5
 * {'': 'a.b.1.c.2.d.e', '': 1} // _id: 5
 */

// Test that a query with multiple numeric path components returns all relevant documents,
// whether the numeric path component refers to a fieldname or array index in each doc:
//
// _id:1 will be captured by the special fieldname-or-array-index bounds 'a.b.c', but will be
// filtered out by the INEXACT_FETCH since it has no array index or fieldname 'b.1'.
// _id:2 will match both 'a.0' and 'b.1' by array index.
// _id:3 will match both 'a.0' and 'b.1' by fieldname.
// _id:4 will match 'a.0' by array index and 'b.1' by fieldname.
// _id:5 is not captured by the special fieldname-or-array-index bounds.
//
// We examine the solution's 'nReturned' versus 'totalDocsExamined' to confirm this.
// totalDocsExamined: [_id:1, _id:2, _id:3, _id:4], nReturned: [_id:2, _id:3, _id:4]
assertWildcardQuery({'a.0.b.1.c': 1},
                    'a.0.b.1.c',
                    {'executionStats.nReturned': 3, 'executionStats.totalDocsExamined': 4});

// Test that we can query a primitive value at a specific array index.
assertWildcardQuery({'a.0.b.1.c.2.d.3': 3},
                    'a.0.b.1.c.2.d.3',
                    {'executionStats.nReturned': 1, 'executionStats.totalDocsExamined': 1});

// Test that a $** index can't be used for a query through more than 8 nested array indices.
assert.commandWorked(
    coll.insert({_id: 6, a: [{b: [{c: [{d: [{e: [{f: [{g: [{h: [{i: [1]}]}]}]}]}]}]}]}]}));
// We can query up to a depth of 8 arrays via specific indices, but not through 9 or more.
assertWildcardQuery({'a.0.b.0.c.0.d.0.e.0.f.0.g.0.h.0.i': 1}, 'a.0.b.0.c.0.d.0.e.0.f.0.g.0.h.0.i');
assertWildcardQuery({'a.0.b.0.c.0.d.0.e.0.f.0.g.0.h.0.i.0': 1}, null);

// Test that a query with multiple positional path components following a multikey component cannot
// use a wildcard index.
assertWildcardQuery({'a.0.1.d': 1}, null);

// Test that fieldname-or-array-index queries do not inappropriately trim predicates; that is,
// all predicates on the field are added to a FETCH filter above the IXSCAN.
assert(coll.drop());
assert.commandWorked(coll.createIndex({"$**": 1}));

assert.commandWorked(coll.insert({_id: 1, a: [0, 1, 2]}));
assert.commandWorked(coll.insert({_id: 2, a: [1, 2, 3]}));
assert.commandWorked(coll.insert({_id: 3, a: [2, 3, 4], b: [5, 6, 7]}));
assert.commandWorked(coll.insert({_id: 4, a: [3, 4, 5], b: [6, 7, 8], c: {'0': 9}}));
assert.commandWorked(coll.insert({_id: 5, a: [4, 5, 6], b: [7, 8, 9], c: {'0': 10}}));
assert.commandWorked(coll.insert({_id: 6, a: [5, 6, 7], b: [8, 9, 10], c: {'0': 11}}));

assertWildcardQuery({"a.0": {$gt: 1, $lt: 4}}, 'a.0', {'executionStats.nReturned': 2});
assertWildcardQuery({"a.1": {$gte: 1, $lte: 4}}, 'a.1', {'executionStats.nReturned': 4});
assertWildcardQuery({"b.2": {$in: [5, 9]}}, 'b.2', {'executionStats.nReturned': 1});
assertWildcardQuery({"c.0": {$in: [10, 11]}}, 'c.0', {'executionStats.nReturned': 2});

// Test that the $** index doesn't trim predicates when planning across multiple nested $and/$or
// expressions on various fieldname-or-array-index paths.
const trimTestQuery = {
    $or: [
        {"a.0": {$gte: 0, $lt: 3}, "a.1": {$in: [2, 3, 4]}},
        {"b.1": {$gt: 6, $lte: 9}, "c.0": {$gt: 9, $lt: 12}}
    ]
};
const trimTestExplain = coll.find(trimTestQuery).explain("executionStats");
// Verify that the expected number of documents were matched, and the $** index was used.
// Matched documents: [_id:2, _id:3, _id:5, _id:6]
assert.eq(trimTestExplain.executionStats.nReturned, 4);
const trimTestIxScans = getPlanStages(trimTestExplain.queryPlanner.winningPlan, "IXSCAN");
for (let ixScan of trimTestIxScans) {
    assert.eq(ixScan.keyPattern["$_path"], 1);
}
// Confirm that a collection scan produces the same results.
assertArrayEq(coll.find(trimTestQuery).toArray(),
              coll.find(trimTestQuery).hint({$natural: 1}).toArray());

assert(coll.drop());
assert.commandWorked(coll.createIndex({"$**": 1}));
assert.commandWorked(coll.insert({a: {0: {1: "exists"}}}));
assert.commandWorked(coll.insert({a: {0: [2, "exists"]}}));
assert.commandWorked(coll.insert({a: {0: [2, {"object_exists": 1}]}}));
assert.commandWorked(coll.insert({a: {0: [2, ["array_exists"]]}}));
assert.commandWorked(coll.insert({a: {0: [{1: "exists"}]}}));
assert.commandWorked(coll.insert({a: {0: [{1: []}]}}));
assert.commandWorked(coll.insert({a: {0: [{1: {}}]}}));
assert.commandWorked(coll.insert({a: {0: ["not_exist"]}}));
assert.commandWorked(coll.insert({a: {"01": ["not_exists"]}}));

// Verify that when "a" is not multikey, a query with multiple successive positional path components
// following "a" can use the wildcard index.
let existenceQuery = {"a.0.1": {$exists: true}};
assertWildcardQuery(existenceQuery, "a.0.1", {"executionStats.nReturned": 7});
assertArrayEq(coll.find(existenceQuery).toArray(),
              coll.find(existenceQuery).hint({$natural: 1}).toArray());

assert.commandWorked(coll.insert({a: [{1: "exists"}, 1]}));
assert.commandWorked(coll.insert({a: [{0: [{1: ["exists"]}]}]}));
assert.commandWorked(coll.insert({a: [{}, {0: [{1: ["exists"]}]}]}));
assert.commandWorked(coll.insert({a: [{}, {0: [[], {}, {1: ["exists"]}]}]}));
assert.commandWorked(coll.insert({a: [{11: "exist"}]}));
assert.commandWorked(coll.insert({a: [{11: {b: "exist"}}]}));

// Verify that an existence query with a positional path component can use the wildcard index.
existenceQuery = {
    "a.0": {$exists: true}
};
assertWildcardQuery(existenceQuery, "a.0", {"executionStats.nReturned": 14});
assertArrayEq(coll.find(existenceQuery).toArray(),
              coll.find(existenceQuery).hint({$natural: 1}).toArray());

// Verify that an existence query with two successive numeric path components, but where one is not
// spelled like a BSON array index, can use a wildcard index.
existenceQuery = {
    "a.01.0": {$exists: true}
};
assertWildcardQuery(existenceQuery, "a.01.0", {"executionStats.nReturned": 1});
assertArrayEq(coll.find(existenceQuery).toArray(),
              coll.find(existenceQuery).hint({$natural: 1}).toArray());

// Verify that multiple successive positional path components preclude use of the wildcard index
// when "a" is multikey.
assertWildcardQuery({"a.0.11": {$exists: true}}, null, {"executionStats.nReturned": 2});
assertWildcardQuery({"a.0.11.b": {$exists: true}}, null, {"executionStats.nReturned": 1});
assertWildcardQuery({"a.3.4": {$exists: true}}, null, {"executionStats.nReturned": 0});
assertWildcardQuery({"a.3.4.b": {$exists: true}}, null, {"executionStats.nReturned": 0});
}());
