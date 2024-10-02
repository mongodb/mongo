/**
 * Validate bounded collection scans on a clustered collection.
 */
import {getExecutionStats, getPlanStage} from "jstests/libs/analyze_plan.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

export const testClusteredCollectionBoundedScan = function(coll, clusterKey) {
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
    // also handles that this stat is in different places for the two engines and when using a
    // sharded cluster:
    //   Classic: executionStats.executionStages.docsExamined
    //   SBE:     executionStats.totalDocsExamined
    //   Sharded: executionStats.totalDocsExamined
    function assertDocsExamined(executionStats, expectedClassic, expectedSbe) {
        let sbe = false;
        let docsExamined = undefined;
        const shards = executionStats.executionStages.shards;

        if (shards == undefined) {
            // single node case
            docsExamined = executionStats.executionStages.docsExamined;
            if (docsExamined == undefined) {
                sbe = true;
                docsExamined = executionStats.totalDocsExamined;
            }
        } else {
            // sharded case
            docsExamined = executionStats.totalDocsExamined;
            if (shards[0].executionStages.docsExamined == undefined) {
                sbe = true;
            }
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
            // use batchSize to avoid selecting EXPRESS instead of CLUSTERED_IXSCAN
            explain: {find: coll.getName(), filter: {[clusterKeyFieldName]: 5}, batchSize: 20},
            verbosity: "executionStats"
        }));

        const clusteredIxScan = getPlanStage(expl, "CLUSTERED_IXSCAN");

        assert(clusteredIxScan);
        assert.eq(5, clusteredIxScan.minRecord);
        assert.eq(5, clusteredIxScan.maxRecord);

        assert.eq(1, expl.executionStats.executionStages.nReturned);

        // On a sharded cluster, assume that the cluster only has one shard.
        const executionStats = getExecutionStats(expl)[0];
        // In Classic, expect nReturned + 1 documents examined by design - additional cursor 'next'
        // beyond the range. In SBE, expect nReturned as it does not examine the extra document.
        assertDocsExamined(executionStats, 2, 1);
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

        const clusteredIxScan = getPlanStage(expl, "CLUSTERED_IXSCAN");
        assert(clusteredIxScan);
        assert(clusteredIxScan.hasOwnProperty("maxRecord"));
        assert.eq(val, clusteredIxScan.maxRecord);

        if (!op.startsWith("$_internal")) {
            // Internal ops do not do type bracketing, so min record would not
            // be expected for $_internalExprLt.
            assert(clusteredIxScan.hasOwnProperty("minRecord"));
            assert.eq(NaN, clusteredIxScan.minRecord);
        }

        assert.eq(expectedNReturned, expl.executionStats.executionStages.nReturned);

        // In this case the scans do not hit EOF, so there is an extra cursor->next() call past the
        // end of the range in Classic, making SBE expect one fewer doc examined than Classic.
        assertDocsExamined(
            expl.executionStats, expectedDocsExaminedClassic, expectedDocsExaminedSbe);
    }

    function testGT(op,
                    val,
                    expectedNReturned,
                    expectedDocsExaminedClassic,
                    expectedDocsExaminedSbe = expectedDocsExaminedClassic) {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {find: coll.getName(), filter: {[clusterKeyFieldName]: {[op]: val}}},
            verbosity: "executionStats"
        }));

        const clusteredIxScan = getPlanStage(expl, "CLUSTERED_IXSCAN");

        assert(clusteredIxScan);
        if (!op.startsWith("$_internal")) {
            // Internal ops do not do type bracketing, so no max record would not
            // be expected for $_internalExprGt.
            assert(clusteredIxScan.hasOwnProperty("maxRecord"));
            assert.eq(Infinity, clusteredIxScan.maxRecord);
        }
        assert(clusteredIxScan.hasOwnProperty("minRecord"));
        assert.eq(val, clusteredIxScan.minRecord);

        assert.eq(expectedNReturned, expl.executionStats.executionStages.nReturned);

        // In this case the scans hit EOF, so there is no extra cursor->next() call in Classic,
        // making Classic and SBE expect the same number of docs examined.
        assertDocsExamined(
            expl.executionStats, expectedDocsExaminedClassic, expectedDocsExaminedSbe);
    }

    function testRange(min,
                       minVal,
                       max,
                       maxVal,
                       expectedNReturned,
                       expectedDocsExaminedClassic,
                       expectedDocsExaminedSbe = expectedDocsExaminedClassic - 1) {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {
                find: coll.getName(),
                filter: {[clusterKeyFieldName]: {[min]: minVal, [max]: maxVal}}
            },
            verbosity: "executionStats"
        }));

        const clusteredIxScan = getPlanStage(expl, "CLUSTERED_IXSCAN");

        assert(clusteredIxScan);
        assert(clusteredIxScan.hasOwnProperty("maxRecord"));
        assert(clusteredIxScan.hasOwnProperty("minRecord"));
        assert.eq(minVal, clusteredIxScan.minRecord);
        assert.eq(maxVal, clusteredIxScan.maxRecord);

        assert.eq(expectedNReturned, expl.executionStats.executionStages.nReturned);

        // In this case the scans do not hit EOF, so there is an extra cursor->next() call past the
        // end of the range in Classic, making SBE expect one fewer doc examined than Classic.
        assertDocsExamined(
            expl.executionStats, expectedDocsExaminedClassic, expectedDocsExaminedSbe);
    }

    function testIn() {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {find: coll.getName(), filter: {[clusterKeyFieldName]: {$in: [10, 20, 30]}}},
            verbosity: "executionStats"
        }));

        const clusteredIxScan = getPlanStage(expl, "CLUSTERED_IXSCAN");

        assert(clusteredIxScan);
        assert.eq(10, clusteredIxScan.minRecord);
        assert.eq(30, clusteredIxScan.maxRecord);

        assert.eq(3, expl.executionStats.executionStages.nReturned);
        // The range scanned is 21 documents. In Classic, expect 'docsExamined' to be one higher by
        // design - additional cursor 'next' beyond the range. In SBE, expect 21 as it does not
        // examine the extra document.
        assertDocsExamined(expl.executionStats, 22, 21);
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

    function testInternalExprBoundedScans() {
        testEq("$_internalExprEq");

        // The IDs expected to be in the collection are:
        // null, 0-99, "foo"
        // Internal operations should not perform type bracketing, so should expect
        // to see the null and "foo" docs; the _non_ internal equivalents _do_
        // perform type bracketing, so should behave as if null and "foo" do not
        // exist.
        testLT("$_internalExprLt", 10, 11, 12);
        testLT("$_internalExprLte", 10, 12, 13);
        testGT("$_internalExprGt", 89, 11, 11);
        testGT("$_internalExprGte", 89, 12, 12);
        testRange("$_internalExprGt", 20, "$_internalExprLt", 40, 19, 20);
        testRange("$_internalExprGte", 20, "$_internalExprLt", 40, 20, 21);
        testRange("$_internalExprGt", 20, "$_internalExprLte", 40, 20, 21);
        testRange("$_internalExprGte", 20, "$_internalExprLte", 40, 21, 22);
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
        // Expect docsExamined == nReturned + 1 due to the by-design additional cursor 'next' beyond
        // the range. The null id is not examined due to the use of bounded seek.
        testLT("$lt", 10, 10, 11);
        // Expect docsExamined == nReturned + 1 due to the by-design additional cursor 'next' beyond
        // the range.
        testLT("$lte", 10, 11, 12);
        // Expect docsExamined == nReturned + 1 due to (not returned, due to type bracketing)
        // "foo" id. Note that unlike the 'testLT' cases, there's no additional cursor 'next' beyond
        // the range because we hit EOF. A forward seek excluding the lower bound is used.
        testGT("$gt", 89, 10, 11, 10);
        // Expect docsExamined == nReturned + 1 due to (not returned, due to type bracketing)
        // "foo" id.
        testGT("$gte", 89, 11, 12, 11);
        // docsExamined reflects the fact that by design we do an additional cursor 'next' beyond
        // the range.
        testRange("$gt", 20, "$lt", 40, 19, 20);
        testRange("$gte", 20, "$lt", 40, 20, 21);
        testRange("$gt", 20, "$lte", 40, 20, 21);
        testRange("$gte", 20, "$lte", 40, 21, 22);
        testIn();

        testNonClusterKeyScan();
        testInternalExprBoundedScans(coll, clusterKey);
    }

    return testBoundedScans(coll, clusterKey);
};
