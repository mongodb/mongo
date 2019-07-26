/**
 * Tests basic index bounds generation and planning for $** indexes.
 *
 * Does not support stepdowns because the test issues getMores, which the stepdown/kill_primary
 * passthroughs will reject.
 *
 * @tags: [does_not_support_stepdowns, assumes_balancer_off]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");     // For getPlanStages.
load("jstests/libs/fixture_helpers.js");  // For isMongos and numberOfShardsForCollection.

// Asserts that the given cursors produce identical result sets.
function assertResultsEq(cursor1, cursor2) {
    while (cursor1.hasNext()) {
        assert(cursor2.hasNext());
        assert.eq(cursor1.next()._id, cursor2.next()._id);
    }
    assert(!cursor2.hasNext());
}

const coll = db.wildcard_index_bounds;
coll.drop();

// Template document which defines the 'schema' of the documents in the test collection.
const templateDoc = {
    a: 0,
    b: {c: 0, d: {e: 0}, f: {}}
};
const pathList = ['a', 'b.c', 'b.d.e', 'b.f'];

// Insert a set of documents into the collection, based on the template document and populated
// with an increasing sequence of values. This is to ensure that the range of values present for
// each field in the dataset is not entirely homogeneous.
for (let i = 0; i < 10; i++) {
    (function populateDoc(doc, value) {
        for (let key in doc) {
            if (typeof doc[key] === 'object')
                value = populateDoc(doc[key], value);
            else
                doc[key] = value++;
        }
        return value;
    })(templateDoc, i);

    assert.commandWorked(coll.insert(templateDoc));
}

// For sharded passthroughs, we need to know the number of shards occupied by the collection.
const numShards = FixtureHelpers.numberOfShardsForCollection(coll);

// Set of operations which will be applied to each field in the index in turn. If the 'bounds'
// property is null, this indicates that the operation is not supported by $** indexes. The
// 'subpathBounds' property indicates whether the bounds for '$_path' are supposed to contain
// all subpaths rather than a single point-interval, i.e. ["path.to.field.", "path.to.field/").
const operationList = [
    {expression: {$gte: 3}, bounds: ['[3.0, inf.0]']},
    {expression: {$gt: 3}, bounds: ['(3.0, inf.0]']},
    {expression: {$lt: 7}, bounds: ['[-inf.0, 7.0)']},
    {expression: {$lte: 7}, bounds: ['[-inf.0, 7.0]']},
    {expression: {$eq: 5}, bounds: ['[5.0, 5.0]']},
    {
        expression: {$in: [3, 5, 7, 9]},
        bounds: ['[3.0, 3.0]', '[5.0, 5.0]', '[7.0, 7.0]', '[9.0, 9.0]']
    },
    {expression: {$exists: true}, bounds: ['[MinKey, MaxKey]'], subpathBounds: true},
    {expression: {$gte: MinKey, $lte: MaxKey}, bounds: ['[MinKey, MaxKey]'], subpathBounds: true},
    {expression: {$exists: false}, bounds: null},
    {expression: {$eq: null}, bounds: null},
    {expression: {$eq: {abc: 1}}, bounds: null},
    {expression: {$lt: {abc: 1}}, bounds: null},
    {expression: {$ne: {abc: 1}}, bounds: null},
    {expression: {$lt: {abc: 1}, $gt: {abc: 1}}, bounds: null},
    {expression: {$in: [{abc: 1}, 1, 2, 3]}, bounds: null},
    {expression: {$in: [null, 1, 2, 3]}, bounds: null},
    {expression: {$ne: null}, bounds: ["[MinKey, MaxKey]"], subpathBounds: true},
    {expression: {$ne: null, $exists: true}, bounds: ["[MinKey, MaxKey]"], subpathBounds: true},
    // In principle we could have tighter bounds for this. See SERVER-36765.
    {expression: {$eq: null, $exists: true}, bounds: ['[MinKey, MaxKey]'], subpathBounds: true},
    {expression: {$eq: []}, bounds: ['[undefined, undefined]', '[[], []]']}
];

// Given a keyPattern and (optional) pathProjection, this function builds a $** index on the
// collection and then tests each of the match expression in the 'operationList' on each indexed
// field in turn. The 'expectedPaths' argument lists the set of paths which we expect to have
// been indexed based on the spec; this function will confirm that only the appropriate paths
// are present in the $** index. Finally, for each match expression it will perform a rooted-$or
// with one predicate on each expected path, and a rooted $and over all predicates and paths.
function runWildcardIndexTest(keyPattern, pathProjection, expectedPaths) {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(
        coll.createIndex(keyPattern, pathProjection ? {wildcardProjection: pathProjection} : {}));

    // The 'expectedPaths' argument is the set of paths which we expect to be indexed, based on
    // the keyPattern and projection. Make sure that the caller has provided this argument.
    assert(expectedPaths);

    // Verify the expected behaviour for every combination of path and operator.
    for (let op of operationList) {
        // Build up a list of operations that will later be used to test rooted $or.
        const multiFieldPreds = [];
        const orQueryBounds = [];

        for (let path of pathList) {
            // The bounds on '$_path' will always include a point-interval on the path, i.e.
            // ["path.to.field", "path.to.field"]. If 'subpathBounds' is 'true' for this
            // operation, then we add bounds that include all subpaths as well, i.e.
            // ["path.to.field.", "path.to.field/")
            const pointPathBound = `["${path}", "${path}"]`;
            const pathBounds =
                op.subpathBounds ? [pointPathBound, `["${path}.", "${path}/")`] : [pointPathBound];
            // {$_path: pathBounds, path.to.field: [[computed bounds]]}
            const expectedBounds = {$_path: pathBounds, [path]: op.bounds};
            const query = {[path]: op.expression};

            // Explain the query, and determine whether an indexed solution is available.
            const ixScans =
                getPlanStages(coll.find(query).explain().queryPlanner.winningPlan, "IXSCAN");

            // If we expect the current path to have been excluded based on the $** keyPattern
            // and projection, or if the current operation is not supported by $** indexes,
            // confirm that no indexed solution was found.
            if (!expectedPaths.includes(path) || op.bounds === null) {
                assert.eq(ixScans.length,
                          0,
                          () => "Bounds check for operation: " + tojson(op) +
                              " failed. Expected no IXSCAN plans to be generated, but got " +
                              tojson(ixScans));
                continue;
            }

            // Verify that the winning plan uses the $** index with the expected bounds.
            assert.eq(ixScans.length, FixtureHelpers.numberOfShardsForCollection(coll));
            assert.docEq(ixScans[0].keyPattern, {$_path: 1, [path]: 1});
            assert.docEq(ixScans[0].indexBounds, expectedBounds);

            // Verify that the results obtained from the $** index are identical to a COLLSCAN.
            // We must explicitly hint the wildcard index, because we also sort on {_id: 1} to
            // ensure that both result sets are in the same order.
            assertResultsEq(coll.find(query).sort({_id: 1}).hint(keyPattern),
                            coll.find(query).sort({_id: 1}).hint({$natural: 1}));

            // Push the query into the $or and $and predicate arrays.
            orQueryBounds.push(expectedBounds);
            multiFieldPreds.push(query);
        }

        // If the current operation could not use the $** index, skip to the next op.
        if (multiFieldPreds.length === 0) {
            continue;
        }

        // Perform a rooted $or for this operation across all indexed fields; for instance:
        // {$or: [{a: {$eq: 25}}, {'b.c': {$eq: 25}}, {'b.d.e': {$eq: 25}}]}.
        const explainedOr = assert.commandWorked(coll.find({$or: multiFieldPreds}).explain());

        // Obtain the list of index bounds from each individual IXSCAN stage across all shards.
        const ixScanBounds = getPlanStages(explainedOr.queryPlanner.winningPlan, "IXSCAN")
                                 .map(elem => elem.indexBounds);

        // We should find that each branch of the $or has used a separate $** sub-index. In the
        // sharded passthroughs, we expect to have 'orQueryBounds' on each shard.
        assert.eq(ixScanBounds.length, orQueryBounds.length * numShards);
        for (let offset = 0; offset < ixScanBounds.length; offset += orQueryBounds.length) {
            const ixBounds = ixScanBounds.slice(offset, offset + orQueryBounds.length);
            orQueryBounds.forEach(
                exBound => assert(ixBounds.some(ixBound => !bsonWoCompare(ixBound, exBound))));
        }

        // Verify that the results obtained from the $** index are identical to a COLLSCAN.
        assertResultsEq(coll.find({$or: multiFieldPreds}).sort({_id: 1}).hint(keyPattern),
                        coll.find({$or: multiFieldPreds}).sort({_id: 1}).hint({$natural: 1}));

        // Perform an $and for this operation across all indexed fields; for instance:
        // {$and: [{a: {$gte: 50}}, {'b.c': {$gte: 50}}, {'b.d.e': {$gte: 50}}]}.
        const explainedAnd = coll.find({$and: multiFieldPreds}).explain();
        const winningIxScan = getPlanStages(explainedAnd.queryPlanner.winningPlan, "IXSCAN");

        // Extract information about the rejected plans. We should have one IXSCAN for each $**
        // candidate that wasn't the winner. Before SERVER-36521 banned them for $** indexes, a
        // number of AND_SORTED plans would also be generated here; we search for these in order
        // to verify that no such plans now exist.
        const rejectedPlans = getRejectedPlans(explainedAnd);
        let rejectedIxScans = [], rejectedAndSorted = [];
        for (let rejectedPlan of rejectedPlans) {
            rejectedAndSorted = rejectedAndSorted.concat(getPlanStages(rejectedPlan, "AND_SORTED"));
            rejectedIxScans = rejectedIxScans.concat(getPlanStages(rejectedPlan, "IXSCAN"));
        }

        // Confirm that no AND_SORTED plans were generated.
        assert.eq(rejectedAndSorted.length, 0);

        // We should find that one of the available $** subindexes has been chosen as the
        // winner, and all other candidate $** indexes are present in 'rejectedPlans'.
        assert.eq(winningIxScan.length, numShards);
        assert.eq(rejectedIxScans.length, numShards * (expectedPaths.length - 1));

        // Verify that each of the IXSCANs have the expected bounds and $_path key.
        for (let ixScan of winningIxScan.concat(rejectedIxScans)) {
            // {$_path: ["['path.to.field', 'path.to.field']"], path.to.field: [[bounds]]}
            const ixScanPath = JSON.parse(ixScan.indexBounds.$_path[0])[0];
            assert.eq(ixScan.indexBounds[ixScanPath], op.bounds);
            assert(expectedPaths.includes(ixScanPath));
        }

        // Verify that the results obtained from the $** index are identical to a COLLSCAN.
        assertResultsEq(coll.find({$and: multiFieldPreds}).sort({_id: 1}).hint(keyPattern),
                        coll.find({$and: multiFieldPreds}).sort({_id: 1}).hint({$natural: 1}));
    }
}

// Test a $** index that indexes the entire document.
runWildcardIndexTest({'$**': 1}, null, ['a', 'b.c', 'b.d.e', 'b.f']);

// Test a $** index on a single subtree.
runWildcardIndexTest({'a.$**': 1}, null, ['a']);
runWildcardIndexTest({'b.$**': 1}, null, ['b.c', 'b.d.e', 'b.f']);
runWildcardIndexTest({'b.d.$**': 1}, null, ['b.d.e']);

// Test a $** index which includes a subset of paths.
runWildcardIndexTest({'$**': 1}, {a: 1}, ['a']);
runWildcardIndexTest({'$**': 1}, {b: 1}, ['b.c', 'b.d.e', 'b.f']);
runWildcardIndexTest({'$**': 1}, {'b.d': 1}, ['b.d.e']);
runWildcardIndexTest({'$**': 1}, {a: 1, 'b.d': 1}, ['a', 'b.d.e']);

// Test a $** index which excludes a subset of paths.
runWildcardIndexTest({'$**': 1}, {a: 0}, ['b.c', 'b.d.e', 'b.f']);
runWildcardIndexTest({'$**': 1}, {b: 0}, ['a']);
runWildcardIndexTest({'$**': 1}, {'b.d': 0}, ['a', 'b.c', 'b.f']);
runWildcardIndexTest({'$**': 1}, {a: 0, 'b.d': 0}, ['b.c', 'b.f']);
})();
