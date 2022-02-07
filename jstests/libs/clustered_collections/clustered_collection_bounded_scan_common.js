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
        // Expect nReturned + 1 documents examined by design - additional cursor 'next' beyond
        // the range.
        assert.eq(2, expl.executionStats.executionStages.docsExamined);
    }
    function testLT(op, val, expectedNReturned, expectedDocsExamined) {
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
        assert.eq(expectedDocsExamined, expl.executionStats.executionStages.docsExamined);
    }
    function testGT(op, val, expectedNReturned, expectedDocsExamined) {
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
        // Expect docsExamined == nReturned + 2 due to the collection scan bounds being always
        // inclusive and due to the by-design additional cursor 'next' beyond the range.
        testLT("$lt", 10, 10, 12);
        // Expect docsExamined == nReturned + 1 due to the by-design additional cursor 'next' beyond
        // the range.
        testLT("$lte", 10, 11, 12);
        // Expect docsExamined == nReturned + 1 due to the collection scan bounds being always
        // inclusive. Note that unlike the 'testLT' cases, there's no additional cursor 'next'
        // beyond the range because we hit EOF.
        testGT("$gt", 89, 10, 11);
        // Expect docsExamined == nReturned.
        testGT("$gte", 89, 11, 11);
        // docsExamined reflects the fact that collection scan bounds are always exclusive and
        // that by design we do an additional cursor 'next' beyond the range.
        testRange("$gt", 20, "$lt", 40, 19, 22);
        testRange("$gte", 20, "$lt", 40, 20, 22);
        testRange("$gt", 20, "$lte", 40, 20, 22);
        testRange("$gte", 20, "$lte", 40, 21, 22);
        testNonClusterKeyScan();
    }

    return testBoundedScans(coll, clusterKey);
};
