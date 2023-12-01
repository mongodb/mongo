/**
 * Validate bounded collection scans on a clustered collection.
 */
import {getPlanStage} from "jstests/libs/analyze_plan.js";
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

    function testEq() {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {find: coll.getName(), filter: {[clusterKeyFieldName]: 5}},
            verbosity: "executionStats"
        }));

        assert(getPlanStage(expl, "CLUSTERED_IXSCAN"));
        assert.eq(5, getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);
        assert.eq(5, getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);

        assert.eq(1, expl.executionStats.executionStages.nReturned);
        // In Classic, expect nReturned + 1 documents examined by design - additional cursor 'next'
        // beyond the range. In SBE, expect nReturned as it does not examine the extra document.
        assertDocsExamined(expl.executionStats, 2, 1);
    }

    function testLT(op, val, expectedNReturned, expectedDocsExaminedClassic) {
        initAndPopulate(coll, clusterKey);

        const expl = assert.commandWorked(coll.getDB().runCommand({
            explain: {find: coll.getName(), filter: {[clusterKeyFieldName]: {[op]: val}}},
            verbosity: "executionStats"
        }));

        assert(getPlanStage(expl, "CLUSTERED_IXSCAN"));
        assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
        assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert.eq(10, getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);
        assert.eq(NaN, getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);

        assert.eq(expectedNReturned, expl.executionStats.executionStages.nReturned);

        // In this case the scans do not hit EOF, so there is an extra cursor->next() call past the
        // end of the range in Classic, making SBE expect one fewer doc examined than Classic.
        assertDocsExamined(
            expl.executionStats, expectedDocsExaminedClassic, expectedDocsExaminedClassic - 1);
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

        assert(getPlanStage(expl, "CLUSTERED_IXSCAN"));
        assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
        assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert.eq(Infinity, getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);
        assert.eq(89, getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);

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

        assert(getPlanStage(expl, "CLUSTERED_IXSCAN"));
        assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
        assert(getPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert.eq(minVal, getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);
        assert.eq(maxVal, getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);

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

        assert(getPlanStage(expl, "CLUSTERED_IXSCAN"));
        assert.eq(10, getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);
        assert.eq(30, getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);

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

    function testBoundedScans(coll, clusterKey) {
        testEq();

        // The last argument of the following calls, 'expectedDocsExaminedClassic', and the specific
        // comments, are for Classic engine. SBE does not have the additional cursor->next() call
        // beyond the range, so in calls to testLT() and testRange() its value will be one lower.
        // This is accounted for by delegations to the assertDocsExamined() helper function.

        // As of SERVER-75604, clustered collection scans can be inclusive or exclusive at either
        // end; the filter does not need to examine a record at the lower bound to then discard it.
        // Expect docsExamined == nReturned + 1 due to the by-design additional cursor 'next' beyond
        // the range.
        testLT("$lt", 10, 10, 11);
        // Expect docsExamined == nReturned + 1 due to the by-design additional cursor 'next' beyond
        // the range.
        testLT("$lte", 10, 11, 12);
        // Expect docsExamined (SBE) == nReturned. Note that unlike the 'testLT' cases, there's no
        // additional cursor 'next' beyond the range because we hit EOF.
        // However, Classic needs to examine and discard the value equal to bound, as this is an
        // exclusive bound.
        testGT("$gt", 89, 10, 11, 10);
        // Expect docsExamined == nReturned.
        testGT("$gte", 89, 11, 11);
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
    }

    return testBoundedScans(coll, clusterKey);
};
