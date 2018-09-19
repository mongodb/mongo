/**
 * Test that $** indexes obey collation rules for document values, while the virtual $_path
 * components stored alongside these values in the index always use simple binary comparison.
 *
 * TODO: SERVER-36198: Move this test back into jstests/core/collation.js
 */
(function() {
    "user strict";

    load("jstests/aggregation/extras/utils.js");  // For arrayEq.
    load("jstests/libs/analyze_plan.js");         // For getPlanStages.
    load("jstests/libs/get_index_helpers.js");    // For GetIndexHelpers.

    const assertArrayEq = (l, r) => assert(arrayEq(l, r));

    const coll = db.all_paths_collation;
    coll.drop();

    // Extracts the winning plan for the given query and projection from the explain output.
    const winningPlan = (query, proj) => coll.find(query, proj).explain().queryPlanner.winningPlan;

    // Runs the given query and confirms that: (1) the $** was used to answer the query, (2) the
    // results produced by the $** index match the given 'expectedResults', and (3) the same output
    // is produced by a COLLSCAN with the same collation.
    function assertAllPathsIndexAnswersQuery(query, expectedResults, projection) {
        // Verify that the $** index can answer this query.
        const ixScans = getPlanStages(winningPlan(query, (projection || {_id: 0})), "IXSCAN");
        assert.gt(ixScans.length, 0, tojson(coll.find(query).explain()));
        ixScans.forEach((ixScan) => assert(ixScan.keyPattern.$_path));

        // Assert that the $** index produces the expected results, and that these are the same
        // as those produced by a COLLSCAN with the same collation.
        const allPathsResults = coll.find(query, (projection || {_id: 0})).toArray();
        assertArrayEq(allPathsResults, expectedResults);
        assertArrayEq(allPathsResults,
                      coll.find(query, (projection || {_id: 0}))
                          .collation({locale: "en_US", strength: 1})
                          .hint({$natural: 1})
                          .toArray());
    }

    // Confirms that the index matching the given keyPattern has the specified collation.
    function assertIndexHasCollation(keyPattern, collation) {
        var indexSpecs = coll.getIndexes();
        var found = GetIndexHelpers.findByKeyPattern(indexSpecs, keyPattern, collation);
        assert.neq(null,
                   found,
                   "Index with key pattern " + tojson(keyPattern) + " and collation " +
                       tojson(collation) + " not found: " + tojson(indexSpecs));
    }

    try {
        // Required in order to build $** indexes.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: true}));

        // Recreate the collection with a default case-insensitive collation.
        assert.commandWorked(
            db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 1}}));

        // Confirm that the $** index inherits the collection's default collation.
        assert.commandWorked(coll.createIndex({"$**": 1}));
        assertIndexHasCollation({"$**": 1}, {
            locale: "en_US",
            caseLevel: false,
            caseFirst: "off",
            strength: 1,
            numericOrdering: false,
            alternate: "non-ignorable",
            maxVariable: "punct",
            normalization: false,
            backwards: false,
            version: "57.1",
        });

        // Insert a series of documents whose fieldnames and values differ only by case.
        assert.commandWorked(coll.insert({a: {b: "string", c: "STRING"}, d: "sTrInG", e: 5}));
        assert.commandWorked(coll.insert({a: {b: "STRING", c: "string"}, d: "StRiNg", e: 5}));
        assert.commandWorked(coll.insert({A: {B: "string", C: "STRING"}, d: "sTrInG", E: 5}));
        assert.commandWorked(coll.insert({A: {B: "STRING", C: "string"}, d: "StRiNg", E: 5}));

        // Confirm that only the document's values adhere to the case-insensitive collation. The
        // field paths, which are also present in the $** index keys, are evaluated using simple
        // binary comparison; so for instance, path "a.b" does *not* match path "A.B".
        assertAllPathsIndexAnswersQuery({"a.b": "string"}, [
            {a: {b: "string", c: "STRING"}, d: "sTrInG", e: 5},
            {a: {b: "STRING", c: "string"}, d: "StRiNg", e: 5}
        ]);
        assertAllPathsIndexAnswersQuery({"A.B": "string"}, [
            {A: {B: "string", C: "STRING"}, d: "sTrInG", E: 5},
            {A: {B: "STRING", C: "string"}, d: "StRiNg", E: 5}
        ]);

        // All documents in the collection are returned if we query over both upper- and lower-case
        // fieldnames, or when the fieldname has a consistent case across all documents.
        const allDocs = coll.find({}, {_id: 0}).toArray();
        assertAllPathsIndexAnswersQuery({$or: [{"a.c": "string"}, {"A.C": "string"}]}, allDocs);
        assertAllPathsIndexAnswersQuery({d: "string"}, allDocs);

        // Confirm that the $** index also differentiates between upper and lower fieldname case
        // when querying fields which do not contain string values.
        assertAllPathsIndexAnswersQuery({e: 5}, [
            {a: {b: "string", c: "STRING"}, d: "sTrInG", e: 5},
            {a: {b: "STRING", c: "string"}, d: "StRiNg", e: 5}
        ]);
        assertAllPathsIndexAnswersQuery({E: 5}, [
            {A: {B: "string", C: "STRING"}, d: "sTrInG", E: 5},
            {A: {B: "STRING", C: "string"}, d: "StRiNg", E: 5}
        ]);

        // Confirm that the $** index produces a covered plan for a query on non-string, non-object,
        // non-array values.
        assert(isIndexOnly(coll.getDB(), winningPlan({e: 5}, {_id: 0, e: 1})));
        assert(isIndexOnly(coll.getDB(), winningPlan({E: 5}, {_id: 0, E: 1})));

        // Confirm that the $** index differentiates fieldname case when attempting to cover.
        assert(!isIndexOnly(coll.getDB(), winningPlan({e: 5}, {_id: 0, E: 1})));
        assert(!isIndexOnly(coll.getDB(), winningPlan({E: 5}, {_id: 0, e: 1})));

        // Confirm that attempting to project the virtual $_path field which is present in $** index
        // keys produces a non-covered solution, which nonetheless returns the correct results.
        assert(!isIndexOnly(coll.getDB(), winningPlan({e: 5}, {_id: 0, e: 1, $_path: 1})));
        assertAllPathsIndexAnswersQuery({e: 5}, [{e: 5}, {e: 5}], {_id: 0, e: 1, $_path: 1});
    } finally {
        // Disable $** indexes once the tests have either completed or failed.
        db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: false});

        // TODO: SERVER-36444 remove calls to drop() once wildcard index validation works.
        coll.drop();
    }
})();
