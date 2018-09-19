/**
 * Tests that queries using a multikey $** index, return correct results.
 *
 * TODO: SERVER-36198: Move this test back to jstests/core/.
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For arrayEq.
    load("jstests/libs/analyze_plan.js");         // For getPlanStages.

    const assertArrayEq = (l, r) => assert(arrayEq(l, r), tojson(l) + " != " + tojson(r));

    const coll = db.all_paths_multikey_index;
    coll.drop();

    // Template document which defines the 'schema' of the documents in the test collection.
    const templateDoc = {a: [], b: {c: [], d: [{e: 0}]}};
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
    function runAllPathsIndexTest(keyPattern, pathProjection, expectedPaths) {
        assert.commandWorked(coll.dropIndexes());
        assert.commandWorked(coll.createIndex(
            keyPattern, pathProjection ? {starPathsTempName: pathProjection} : {}));
        assert(expectedPaths);
        // Verify the expected behaviour for every combination of path and operator.
        for (let op of operationList) {
            for (let path of pathList) {
                const query = {[path]: op.expression};
                // Explain the query, and determine whether an indexed solution is available.
                const explain = assert.commandWorked(coll.find(query).explain());
                const ixScans = getPlanStages(explain.queryPlanner.winningPlan, "IXSCAN");
                // If we expect the current path to have been excluded based on the $** keyPattern
                // or projection, confirm that no indexed solution was found.
                if (!expectedPaths.includes(path)) {
                    assert.eq(ixScans.length, 0, path);
                    continue;
                }
                // Verify that the winning plan uses the $** index with the expected path.
                assert.eq(ixScans.length, 1);
                assert.docEq(ixScans[0].keyPattern, {"$_path": 1, [path]: 1});
                // Verify that the results obtained from the $** index are identical to a COLLSCAN.
                assertArrayEq(coll.find(query).toArray(),
                              coll.find(query).hint({$natural: 1}).toArray());
            }
        }
    }

    // Required in order to build $** indexes.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: true}));
    try {
        // Test a $** index that indexes the entire document.
        runAllPathsIndexTest({'$**': 1}, null, ['a', 'b.c', 'b.d.e']);
        // Test a $** index on a single subtree.
        runAllPathsIndexTest({'a.$**': 1}, null, ['a']);
        runAllPathsIndexTest({'b.$**': 1}, null, ['b.c', 'b.d.e']);
        runAllPathsIndexTest({'b.c.$**': 1}, null, ['b.c']);
        runAllPathsIndexTest({'b.d.$**': 1}, null, ['b.d.e']);
        // Test a $** index which includes a subset of paths.
        runAllPathsIndexTest({'$**': 1}, {a: 1}, ['a']);
        runAllPathsIndexTest({'$**': 1}, {b: 1}, ['b.c', 'b.d.e']);
        runAllPathsIndexTest({'$**': 1}, {'b.d': 1}, ['b.d.e']);
        runAllPathsIndexTest({'$**': 1}, {a: 1, 'b.d': 1}, ['a', 'b.d.e']);
        // Test a $** index which excludes a subset of paths.
        runAllPathsIndexTest({'$**': 1}, {a: 0}, ['b.c', 'b.d.e']);
        runAllPathsIndexTest({'$**': 1}, {b: 0}, ['a']);
        runAllPathsIndexTest({'$**': 1}, {'b.c': 0}, ['a', 'b.d.e']);
        runAllPathsIndexTest({'$**': 1}, {a: 0, 'b.c': 0}, ['b.d.e']);

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
        assert.eq(0,
                  coll.find({"b.c.d": {$elemMatch: {"e.f": {$gt: 0, $lt: 9}}}})
                      .hint({$natural: 1})
                      .itcount());
    } finally {
        // Disable $** indexes once the tests have either completed or failed.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: false}));
    }
})();
