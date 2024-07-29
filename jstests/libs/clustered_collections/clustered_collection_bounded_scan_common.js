/**
 * Validate bounded collection scans on a clustered collection.
 */

const testClusteredCollectionBoundedScan = function(coll, clusterKey) {
    "use strict";
    load("jstests/libs/analyze_plan.js");
    load("jstests/libs/collection_drop_recreate.js");

    const batchSize = 100;
    const clusterKeyFieldName = Object.keys(clusterKey)[0];

    function initAndPopulate(coll, clusterKey) {
        const clusterKeyFieldName = Object.keys(clusterKey)[0];
        assertDropCollection(coll.getDB(), coll.getName());
        assert.commandWorked(coll.getDB().createCollection(
            coll.getName(), {clusteredIndex: {key: clusterKey, unique: true}}));

        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < batchSize; i++) {
            bulk.insert({[clusterKeyFieldName]: i, a: -i});
        }

        assert.commandWorked(bulk.execute());
        assert.eq(coll.find().itcount(), batchSize);

        // Now add additional documents with IDs of a different type.
        // Normal exprs should be type bracketed, and should never
        // see these documents.
        // Internal ops are not type bracketed, and will see these
        // documents.
        const extra = coll.initializeUnorderedBulkOp();
        // `null` should sort before ints.
        extra.insert({[clusterKeyFieldName]: null, a: null});
        // And strings should sort after.
        extra.insert({[clusterKeyFieldName]: "foo", a: "foo"});
        assert.commandWorked(extra.execute());
    }

    // Checks that the number of docs examined matches the expected number. There are separate
    // expected args for Classic vs SBE because in Classic there is an extra cursor->next() call
    // beyond the end of the range if EOF has not been hit, but in SBE there is not. This function
    // also handles that this stat is in different places for the two engines:
    //   Classic: executionStats.executionStages.docsExamined
    //   SBE:     executionStats.totalDocsExamined
    function assertDocsExamined(executionStats, expectedClassic, expectedSbe) {
        let sbe = false;
        let docsExamined = executionStats.executionStages.docsExamined;
        if (docsExamined == undefined) {
            sbe = true;
            docsExamined = executionStats.totalDocsExamined;
        }
        if (sbe) {
            assert.eq(expectedSbe, docsExamined);
        } else {
            assert.eq(expectedClassic, docsExamined);
        }
    }

    function testEq(op = "$eq") {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {find: coll.getName(), filter: {[clusterKeyFieldName]: 5}},
            verbosity: "executionStats"
        }));

        assert(getPlanStage(expl, "CLUSTERED_IXSCAN"));
        assert.eq(5, getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);
        assert.eq(5, getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);

        assert.eq(1, expl.executionStats.executionStages.nReturned);
        // Expect nReturned + 1 documents examined by design - additional cursor 'next' beyond
        // the range.
        assert.eq(2, expl.executionStats.executionStages.docsExamined);
    }

    function testLT(op,
                    val,
                    expectedNReturned,
                    expectedDocsExaminedClassic,
                    expectedDocsExaminedSbe = expectedDocsExaminedClassic - 1) {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {find: coll.getName(), filter: {[clusterKeyFieldName]: {[op]: val}}},
            verbosity: "executionStats"
        }));

        assert(getPlanStage(expl, "CLUSTERED_IXSCAN"));
        assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
        assert.eq(val, getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);

        if (!op.startsWith("$_internal")) {
            // Internal ops do not do type bracketing, so min record would not
            // be expected for $_internalExprLt.
            assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
            assert.eq(NaN, getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);
        }

        assert.eq(expectedNReturned, expl.executionStats.executionStages.nReturned);

        // In this case the scans do not hit EOF, so there is an extra cursor->next() call past the
        // end of the range in Classic, making SBE expect one fewer doc examined than Classic.
        assertDocsExamined(
            expl.executionStats, expectedDocsExaminedClassic, expectedDocsExaminedSbe);
    }
    function testGT(op, val, expectedNReturned, expectedDocsExamined) {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {find: coll.getName(), filter: {[clusterKeyFieldName]: {[op]: val}}},
            verbosity: "executionStats"
        }));

        assert(getPlanStage(expl, "CLUSTERED_IXSCAN"));
        if (!op.startsWith("$_internal")) {
            // Internal ops do not do type bracketing, so no max record would not
            // be expected for $_internalExprGt.
            assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
            assert.eq(Infinity, getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);
        }
        assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert.eq(val, getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);

        assert.eq(expectedNReturned, expl.executionStats.executionStages.nReturned);
        assert.eq(expectedDocsExamined, expl.executionStats.executionStages.docsExamined);
    }
    function testRange(min, minVal, max, maxVal, expectedNReturned, expectedDocsExamined) {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {
                find: coll.getName(),
                filter: {[clusterKeyFieldName]: {[min]: minVal, [max]: maxVal}}
            },
            verbosity: "executionStats"
        }));

        assert(getPlanStage(expl, "CLUSTERED_IXSCAN"));
        assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
        assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert.eq(minVal, getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);
        assert.eq(maxVal, getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);

        assert.eq(expectedNReturned, expl.executionStats.executionStages.nReturned);
        assert.eq(expectedDocsExamined, expl.executionStats.executionStages.docsExamined);
    }
    function testIn() {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {find: coll.getName(), filter: {[clusterKeyFieldName]: {$in: [10, 20, 30]}}},
            verbosity: "executionStats"
        }));

        assert(getPlanStage(expl, "CLUSTERED_IXSCAN"));
        assert.eq(10, getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);
        assert.eq(30, getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);

        assert.eq(3, expl.executionStats.executionStages.nReturned);
        // The range scanned is 21 documents + 1 extra document by design - additional cursor
        // 'next' beyond the range.
        assert.eq(22, expl.executionStats.executionStages.docsExamined);
    }
    function testNonClusterKeyScan() {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {find: coll.getName(), filter: {a: {$gt: -10}}},
            verbosity: "executionStats"
        }));

        assert(getPlanStage(expl, "COLLSCAN"));
        assert(!getPlanStage(expl, "COLLSCAN").hasOwnProperty("maxRecord"));
        assert(!getPlanStage(expl, "COLLSCAN").hasOwnProperty("minRecord"));
        assert.eq(10, expl.executionStats.executionStages.nReturned);
    }

    function testInternalExprBoundedScans(coll, clusterKey) {
        testEq("$_internalExprEq");

        // The IDs expected to be in the collection are:
        // null, 0-99, "foo"
        // Internal operations should not perform type bracketing, so should expect
        // to see the null and "foo" docs; the _non_ internal equivalents _do_
        // perform type bracketing, so should behave as if null and "foo" do not
        // exist.
        testLT("$_internalExprLt", 10, 11, 12);
        testLT("$_internalExprLte", 10, 12, 13);
        testGT("$_internalExprGt", 89, 11, 12, 11);
        testGT("$_internalExprGte", 89, 12, 12);
        testRange("$_internalExprGt", 20, "$_internalExprLt", 40, 19, 21, 19);
        testRange("$_internalExprGte", 20, "$_internalExprLt", 40, 20, 21, 20);
        testRange("$_internalExprGt", 20, "$_internalExprLte", 40, 20, 22, 20);
        testRange("$_internalExprGte", 20, "$_internalExprLte", 40, 21, 22, 21);
    }

    function testBoundedScans(coll, clusterKey) {
        testEq();

        // Expected set of IDs:
        // null, 0-99, "foo"

        // The last argument of the following calls, 'expectedDocsExaminedClassic', and the specific
        // comments, are for Classic engine. SBE does not have the additional cursor->next() call
        // beyond the range, so in calls to testLT() and testRange() its value will be one lower.
        // This is accounted for by delegations to the assertDocsExamined() helper function.

        // As of SERVER-75604, clustered collection scans can be inclusive or exclusive at either
        // end; the filter does not need to examine a record at the lower bound to then discard it.
        // Expect docsExamined == nReturned + 1 + 1 due to (not returned, due to type bracketing)
        // null id, and the by-design additional cursor 'next' beyond the range.
        testLT("$lt", 10, 10, 12, 10);
        // Expect docsExamined == nReturned + 1 + 1 due to (not returned, due to type bracketing)
        // null id, and the by-design additional cursor 'next' beyond the range.
        testLT("$lte", 10, 11, 13, 11);
        // Expect docsExamined == nReturned + 1 + 1 due to (not returned, due to type bracketing)
        // "foo" id. Note that unlike the 'testLT' cases, there's no additional cursor 'next' beyond
        // the range because we hit EOF. However, Classic needs to examine and discard the value
        // equal to bound, as this is an exclusive bound.
        testGT("$gt", 89, 10, 12, 10);
        // Expect docsExamined == nReturned + 1 due to (not returned, due to type bracketing)
        // "foo" id.
        testGT("$gte", 89, 11, 12, 11);
        // docsExamined reflects the fact that by design we do an additional cursor 'next' beyond
        // the range.
        // In addition, Classic needs to examine and discard the value equal to bound,
        // while SBE's scan starts after the bound.
        testRange("$gt", 20, "$lt", 40, 19, 21, 19);
        testRange("$gte", 20, "$lt", 40, 20, 21, 20);
        // Again, Classic and SBE differ by 2 for the above reasons.
        testRange("$gt", 20, "$lte", 40, 20, 22, 20);
        testRange("$gte", 20, "$lte", 40, 21, 22, 21);
        testIn();

        testNonClusterKeyScan();
        testInternalExprBoundedScans(coll, clusterKey);
    }

    return testBoundedScans(coll, clusterKey);
};
