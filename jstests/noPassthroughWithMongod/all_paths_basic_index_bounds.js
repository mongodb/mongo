/**
 * Tests basic index bounds generation and planning for $** indexes.
 *
 * Tagged as 'assumes_unsharded_collection' so that the expected relationship between the number of
 * IXSCANs in the explain output and the number of fields in the indexed documents is not distorted
 * by being spread across multiple shards.
 *
 * @tags: [assumes_unsharded_collection]
 *
 * TODO: SERVER-36198: Move this test back to jstests/core/
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For arrayEq.
    load("jstests/libs/analyze_plan.js");         // For getPlanStages.

    const assertArrayEq = (l, r) => assert(arrayEq(l, r));

    const coll = db.all_paths_index_bounds;
    coll.drop();

    // Template document which defines the 'schema' of the documents in the test collection.
    const templateDoc = {a: 0, b: {c: 0, d: {e: 0}, f: {}}};
    const pathList = ['a', 'b.c', 'b.d.e', 'b.f'];

    // Insert a set of documents into the collection, based on the template document and populated
    // with an increasing sequence of values. This is to ensure that the range of values present for
    // each field in the dataset is not entirely homogeneous.
    for (let i = 0; i < 200; i++) {
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

    // Set of operations which will be applied to each field in the index in turn. If the 'bounds'
    // property is null, this indicates that the operation is not supported by $** indexes. The
    // 'subpathBounds' property indicates whether the bounds for '$_path' are supposed to contain
    // all subpaths rather than a single point-interval, i.e. ["path.to.field.", "path.to.field/").
    const operationList = [
        {expression: {$gte: 50}, bounds: ['[50.0, inf.0]']},
        {expression: {$gt: 50}, bounds: ['(50.0, inf.0]']},
        {expression: {$lt: 150}, bounds: ['[-inf.0, 150.0)']},
        {expression: {$lte: 150}, bounds: ['[-inf.0, 150.0]']},
        {expression: {$eq: 75}, bounds: ['[75.0, 75.0]']},
        {
          expression: {$in: [25, 75, 125, 175]},
          bounds: ['[25.0, 25.0]', '[75.0, 75.0]', '[125.0, 125.0]', '[175.0, 175.0]']
        },
        {expression: {$exists: true}, bounds: ['[MinKey, MaxKey]'], subpathBounds: true},
        {
          expression: {$gte: MinKey, $lte: MaxKey},
          bounds: ['[MinKey, MaxKey]'],
          subpathBounds: true
        },
        {expression: {$exists: false}, bounds: null},
        {expression: {$eq: null}, bounds: null},
        {expression: {$ne: null}, bounds: null},
        {expression: {$ne: null, $exists: true}, bounds: ['[MinKey, MaxKey]'], subpathBounds: true},
        // In principle we could have tighter bounds for this. See SERVER-36765.
        {expression: {$eq: null, $exists: true}, bounds: ['[MinKey, MaxKey]'], subpathBounds: true},
    ];

    // Given a keyPattern and (optional) pathProjection, this function builds a $** index on the
    // collection and then tests each of the match expression in the 'operationList' on each indexed
    // field in turn. The 'expectedPaths' argument lists the set of paths which we expect to have
    // been indexed based on the spec; this function will confirm that only the appropriate paths
    // are present in the $** index. Finally, for each match expression it will perform a rooted-$or
    // with one predicate on each expected path, and a rooted $and over all predicates and paths.
    function runAllPathsIndexTest(keyPattern, pathProjection, expectedPaths) {
        assert.commandWorked(coll.dropIndexes());
        assert.commandWorked(coll.createIndex(
            keyPattern, pathProjection ? {starPathsTempName: pathProjection} : {}));

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
                const pathBounds = op.subpathBounds ? [pointPathBound, `["${path}.", "${path}/")`]
                                                    : [pointPathBound];
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
                    assert.eq(ixScans.length, 0);
                    continue;
                }

                // Verify that the winning plan uses the $** index with the expected bounds.
                assert.eq(ixScans.length, 1);
                assert.docEq(ixScans[0].keyPattern, {$_path: 1, [path]: 1});
                assert.docEq(ixScans[0].indexBounds, expectedBounds);

                // Verify that the results obtained from the $** index are identical to a COLLSCAN.
                assertArrayEq(coll.find(query).toArray(),
                              coll.find(query).hint({$natural: 1}).toArray());

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
            const ixScans = getPlanStages(
                coll.find({$or: multiFieldPreds}).explain().queryPlanner.winningPlan, "IXSCAN");

            // We should find that each branch of the $or has used a separate $** sub-index.
            const ixScanBounds = [];
            ixScans.forEach((ixScan) => ixScanBounds.push(ixScan.indexBounds));
            assertArrayEq(ixScanBounds, orQueryBounds);

            // Verify that the results obtained from the $** index are identical to a COLLSCAN.
            assertArrayEq(coll.find({$or: multiFieldPreds}).toArray(),
                          coll.find({$or: multiFieldPreds}).hint({$natural: 1}).toArray());

            // Perform an $and for this operation across all indexed fields; for instance:
            // {$and: [{a: {$gte: 50}}, {'b.c': {$gte: 50}}, {'b.d.e': {$gte: 50}}]}.
            const explainOutput = coll.find({$and: multiFieldPreds}).explain();
            const winningIxScan = getPlanStages(explainOutput.queryPlanner.winningPlan, "IXSCAN");

            // Extract information about the rejected plans. We should have one IXSCAN for each $**
            // candidate that wasn't the winner. Before SERVER-36521 banned them for $** indexes, a
            // number of AND_SORTED plans would also be generated here; we search for these in order
            // to verify that no such plans now exist.
            let rejectedIxScans = [], rejectedAndSorted = [];
            for (let rejectedPlan of explainOutput.queryPlanner.rejectedPlans) {
                rejectedAndSorted =
                    rejectedAndSorted.concat(getPlanStages(rejectedPlan, "AND_SORTED"));
                rejectedIxScans = rejectedIxScans.concat(getPlanStages(rejectedPlan, "IXSCAN"));
            }

            // Confirm that no AND_SORTED plans were generated.
            assert.eq(rejectedAndSorted.length, 0);

            // We should find that one of the available $** subindexes has been chosen as the
            // winner, and all other candidate $** indexes are present in 'rejectedPlans'.
            assert.eq(winningIxScan.length, 1);
            assert.eq(rejectedIxScans.length, expectedPaths.length - 1);

            // Verify that each of the IXSCANs have the expected bounds and $_path key.
            for (let ixScan of winningIxScan.concat(rejectedIxScans)) {
                // {$_path: ["['path.to.field', 'path.to.field']"], path.to.field: [[bounds]]}
                const ixScanPath = JSON.parse(ixScan.indexBounds.$_path[0])[0];
                assert.eq(ixScan.indexBounds[ixScanPath], op.bounds);
                assert(expectedPaths.includes(ixScanPath));
            }

            // Verify that the results obtained from the $** index are identical to a COLLSCAN.
            assertArrayEq(coll.find({$and: multiFieldPreds}).toArray(),
                          coll.find({$and: multiFieldPreds}).hint({$natural: 1}).toArray());
        }
    }

    // Required in order to build $** indexes.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: true}));

    try {
        // Test a $** index that indexes the entire document.
        runAllPathsIndexTest({'$**': 1}, null, ['a', 'b.c', 'b.d.e', 'b.f']);

        // Test a $** index on a single subtree.
        runAllPathsIndexTest({'a.$**': 1}, null, ['a']);
        runAllPathsIndexTest({'b.$**': 1}, null, ['b.c', 'b.d.e', 'b.f']);
        runAllPathsIndexTest({'b.d.$**': 1}, null, ['b.d.e']);

        // Test a $** index which includes a subset of paths.
        runAllPathsIndexTest({'$**': 1}, {a: 1}, ['a']);
        runAllPathsIndexTest({'$**': 1}, {b: 1}, ['b.c', 'b.d.e', 'b.f']);
        runAllPathsIndexTest({'$**': 1}, {'b.d': 1}, ['b.d.e']);
        runAllPathsIndexTest({'$**': 1}, {a: 1, 'b.d': 1}, ['a', 'b.d.e']);

        // Test a $** index which excludes a subset of paths.
        runAllPathsIndexTest({'$**': 1}, {a: 0}, ['b.c', 'b.d.e', 'b.f']);
        runAllPathsIndexTest({'$**': 1}, {b: 0}, ['a']);
        runAllPathsIndexTest({'$**': 1}, {'b.d': 0}, ['a', 'b.c', 'b.f']);
        runAllPathsIndexTest({'$**': 1}, {a: 0, 'b.d': 0}, ['b.c', 'b.f']);
    } finally {
        // Disable $** indexes once the tests have either completed or failed.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: false}));
    }
})();
