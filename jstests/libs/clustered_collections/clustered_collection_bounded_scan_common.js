/**
 * Validate bounded collection scans on a clustered collection.
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {profilerHasAtLeastOneMatchingEntryOrThrow} from "jstests/libs/profiler.js";
import {
    getExecutionStages,
    getPlanStage,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";

export const testClusteredCollectionBoundedScan = function (coll, clusterKey, checkProfile) {
    const batchSize = 100;
    const clusterKeyFieldName = Object.keys(clusterKey)[0];

    function initAndPopulate(coll, clusterKey, numericIds) {
        numericIds = numericIds || Array.from({length: batchSize}, (_, i) => i);
        const clusterKeyFieldName = Object.keys(clusterKey)[0];
        assertDropCollection(coll.getDB(), coll.getName());
        assert.commandWorked(
            coll.getDB().createCollection(coll.getName(), {
                clusteredIndex: {key: clusterKey, unique: true},
            }),
        );

        const bulk = coll.initializeUnorderedBulkOp();
        for (const i of numericIds) {
            bulk.insert({[clusterKeyFieldName]: i, a: -i});
        }

        assert.commandWorked(bulk.execute());
        assert.eq(coll.find().itcount(), numericIds.length);

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

        if (checkProfile) {
            assert.commandWorked(coll.getDB().setProfilingLevel(2, {slowms: 0}));
        }
    }

    function assertLogAndProfileHaveCorrectStage(db, comment, expectedStage) {
        if (!checkProfile) {
            return;
        }

        const slowQueryLogs = assert
            .commandWorked(db.adminCommand({getLog: "global"}))
            .log.map(JSON.parse)
            .filter((entry) => {
                return (
                    entry.msg == "Slow query" &&
                    !entry?.attr?.command?.explain &&
                    entry?.attr?.command?.comment === comment
                );
            });
        assert.gte(slowQueryLogs.length, 1);
        for (let slowQueryLog of slowQueryLogs) {
            assert.eq(slowQueryLog.attr.planSummary, expectedStage, tojson(slowQueryLog));
        }

        profilerHasAtLeastOneMatchingEntryOrThrow({
            profileDB: db,
            filter: {"command.comment": comment, "planSummary": expectedStage},
            errorMsgFilter: {"command.comment": comment},
        });
    }

    // Checks that the number of docs examined matches the expected number. There are separate
    // expected args for Classic vs SBE because in Classic there is an extra cursor->next() call
    // beyond the end of the range if EOF has not been hit, but in SBE there is not.
    function assertDocsExamined(expl, expectedClassic, expectedSbe) {
        const sbe = "slotBasedPlan" in expl.queryPlanner.winningPlan;
        const docsExamined = expl.executionStats.totalDocsExamined;

        if (sbe) {
            assert.eq(expectedSbe, docsExamined, expl.executionStats);
        } else {
            assert.eq(expectedClassic, docsExamined, expl.executionStats);
        }
    }

    function testEq(op = "$eq") {
        initAndPopulate(coll, clusterKey);

        const filter = {[clusterKeyFieldName]: 5};
        const comment = "testEq-" + op;
        // Use 'batchSize' to avoid selecting "EXPRESS" instead of "CLUSTERED_IXSCAN".
        assert.eq(coll.find(filter).batchSize(20).comment(comment).itcount(), 1);
        assertLogAndProfileHaveCorrectStage(coll.getDB(), comment, "CLUSTERED_IXSCAN");
        const expl = assert.commandWorked(
            coll.getDB().runCommand({
                explain: {find: coll.getName(), filter: filter, batchSize: 20},
                verbosity: "executionStats",
            }),
        );

        const clusteredIxScan = getPlanStage(expl, "CLUSTERED_IXSCAN");

        assert(clusteredIxScan);
        assert.eq(5, clusteredIxScan.minRecord);
        assert.eq(5, clusteredIxScan.maxRecord);

        assert.eq(1, expl.executionStats.executionStages.nReturned);

        // In Classic, expect nReturned + 1 documents examined by design - additional cursor 'next'
        // beyond the range. In SBE, expect nReturned as it does not examine the extra document.
        assertDocsExamined(expl, 2, 1);
    }

    function testLT(
        op,
        val,
        expectedNReturned,
        expectedDocsExaminedClassic,
        expectedDocsExaminedSbe = expectedDocsExaminedClassic - 1,
    ) {
        initAndPopulate(coll, clusterKey);

        const filter = {[clusterKeyFieldName]: {[op]: val}};
        const comment = "testLT-" + op + "-" + val;
        assert.eq(coll.find(filter).comment(comment).itcount(), expectedNReturned);
        assertLogAndProfileHaveCorrectStage(coll.getDB(), comment, "CLUSTERED_IXSCAN");
        const expl = assert.commandWorked(
            coll.getDB().runCommand({
                explain: {find: coll.getName(), filter: filter},
                verbosity: "executionStats",
            }),
        );

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
        assertDocsExamined(expl, expectedDocsExaminedClassic, expectedDocsExaminedSbe);
    }

    function testGT(
        op,
        val,
        expectedNReturned,
        expectedDocsExaminedClassic,
        expectedDocsExaminedSbe = expectedDocsExaminedClassic,
    ) {
        initAndPopulate(coll, clusterKey);

        const filter = {[clusterKeyFieldName]: {[op]: val}};
        const comment = "testGT-" + op + "-" + val;
        assert.eq(coll.find(filter).comment(comment).itcount(), expectedNReturned);
        assertLogAndProfileHaveCorrectStage(coll.getDB(), comment, "CLUSTERED_IXSCAN");
        const expl = assert.commandWorked(
            coll.getDB().runCommand({
                explain: {find: coll.getName(), filter: filter},
                verbosity: "executionStats",
            }),
        );

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
        assertDocsExamined(expl, expectedDocsExaminedClassic, expectedDocsExaminedSbe);
    }

    function testRange(
        min,
        minVal,
        max,
        maxVal,
        expectedNReturned,
        expectedDocsExaminedClassic,
        expectedDocsExaminedSbe = expectedDocsExaminedClassic - 1,
    ) {
        initAndPopulate(coll, clusterKey);

        const filter = {[clusterKeyFieldName]: {[min]: minVal, [max]: maxVal}};
        const comment = "testRange-" + min + "-" + minVal + "-" + max + "-" + maxVal;
        assert.eq(coll.find(filter).comment(comment).itcount(), expectedNReturned);
        assertLogAndProfileHaveCorrectStage(coll.getDB(), comment, "CLUSTERED_IXSCAN");
        const expl = assert.commandWorked(
            coll.getDB().runCommand({
                explain: {find: coll.getName(), filter: filter},
                verbosity: "executionStats",
            }),
        );

        const clusteredIxScan = getPlanStage(expl, "CLUSTERED_IXSCAN");

        assert(clusteredIxScan);
        assert(clusteredIxScan.hasOwnProperty("maxRecord"));
        assert(clusteredIxScan.hasOwnProperty("minRecord"));
        assert.eq(minVal, clusteredIxScan.minRecord);
        assert.eq(maxVal, clusteredIxScan.maxRecord);

        assert.eq(expectedNReturned, expl.executionStats.executionStages.nReturned);

        // In this case the scans do not hit EOF, so there is an extra cursor->next() call past the
        // end of the range in Classic, making SBE expect one fewer doc examined than Classic.
        assertDocsExamined(expl, expectedDocsExaminedClassic, expectedDocsExaminedSbe);
    }

    function testIn() {
        initAndPopulate(coll, clusterKey);

        const filter = {[clusterKeyFieldName]: {$in: [10, 20, 30]}};
        const comment = "testIn";
        assert.eq(coll.find(filter).comment(comment).itcount(), 3);
        assertLogAndProfileHaveCorrectStage(coll.getDB(), comment, "CLUSTERED_IXSCAN");

        // Each branch of the $in has a corresponding document in the collection.
        // The classic engine scans two documents per branch (one desired + one overshoot).
        const expectedIds = [10, 20, 30];
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 3,
            classicExpectedSeeksBackward: 3,
            expectedDocsExaminedClassic: 6,
            expectedDocsExaminedSbe: 3,
            expectedRanges: [
                {min: 10, minInclusive: true, max: 10, maxInclusive: true},
                {min: 20, minInclusive: true, max: 20, maxInclusive: true},
                {min: 30, minInclusive: true, max: 30, maxInclusive: true},
            ],
        });
    }

    function testNonClusterKeyScan() {
        initAndPopulate(coll, clusterKey);

        const filter = {a: {$gt: -10}};
        const comment = "testNonClusteredKeyScan";
        assert.eq(coll.find(filter).comment(comment).itcount(), 10);
        assertLogAndProfileHaveCorrectStage(coll.getDB(), comment, "COLLSCAN");

        const expl = assert.commandWorked(
            coll.getDB().runCommand({
                explain: {find: coll.getName(), filter: filter},
                verbosity: "executionStats",
            }),
        );

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

    // Verifies that a backward (descending) bounded scan returns the same results as a forward
    // (ascending) scan, just in reverse order. This catches bugs where ScanBoundInclusion bits
    // are not correctly mapped for backward scans (see SERVER-121822).
    function assertForwardBackwardConsistency(filter, comment) {
        const forward = coll
            .find(filter)
            .sort({[clusterKeyFieldName]: 1})
            .toArray();
        const backward = coll
            .find(filter)
            .sort({[clusterKeyFieldName]: -1})
            .toArray();

        assert.docEq(
            forward,
            backward.reverse(),
            comment + ": backward scan results do not match forward scan results (reversed)",
        );
    }

    function testBackwardBoundedScans() {
        initAndPopulate(coll, clusterKey);

        // Equality.
        assertForwardBackwardConsistency({[clusterKeyFieldName]: {$eq: 50}}, "backward $eq");

        // Single exclusive bound.
        assertForwardBackwardConsistency({[clusterKeyFieldName]: {$lt: 10}}, "backward $lt");
        assertForwardBackwardConsistency({[clusterKeyFieldName]: {$gt: 89}}, "backward $gt");

        // Single inclusive bound.
        assertForwardBackwardConsistency({[clusterKeyFieldName]: {$lte: 10}}, "backward $lte");
        assertForwardBackwardConsistency({[clusterKeyFieldName]: {$gte: 89}}, "backward $gte");

        // Both bounds exclusive.
        assertForwardBackwardConsistency(
            {[clusterKeyFieldName]: {$gt: 20, $lt: 40}},
            "backward $gt/$lt",
        );

        // Both bounds inclusive.
        assertForwardBackwardConsistency(
            {[clusterKeyFieldName]: {$gte: 20, $lte: 40}},
            "backward $gte/$lte",
        );

        // Mixed bounds.
        assertForwardBackwardConsistency(
            {[clusterKeyFieldName]: {$gte: 20, $lt: 40}},
            "backward $gte/$lt",
        );
        assertForwardBackwardConsistency(
            {[clusterKeyFieldName]: {$gt: 20, $lte: 40}},
            "backward $gt/$lte",
        );
    }

    // Verifies that a given query returns exactly the documents with cluster key values
    // in 'expectedIds' (sorted ascending) using the given number of seeks.

    // If `expectedRanges` is given, verifies that the 'recordIdRanges/minRecord/maxRecord' fields in the
    // queryPlanner explain section matches it. Pass [] for trivially-empty scans.
    //
    // The 'recordIdRanges', seek-count and docs-examined assertions are multi-range-specific
    // and only run when the multi-range clustered collscan feature flag is enabled.
    // triviallyEmpty adapts to the value of the feature flag as well.
    // Direction-specific expected docsExamined counts override the non-directional one.
    function assertMultiRangeResults({
        filter,
        expectedIds,
        classicExpectedSeeksForward,
        classicExpectedSeeksBackward,
        expectedDocsExaminedClassic,
        expectedDocsExaminedClassicForward,
        expectedDocsExaminedClassicBackward,
        expectedDocsExaminedSbe,
        triviallyEmpty,
        collation,
        min,
        max,
        expectedRanges,
    }) {
        // If per-direction classic docsExamined values are not given, fall back to the single
        // 'expectedDocsExaminedClassic' for both directions.
        expectedDocsExaminedClassicForward ??= expectedDocsExaminedClassic;
        expectedDocsExaminedClassicBackward ??= expectedDocsExaminedClassic;
        if (Object.keys(filter).length === 1 && "$or" in filter) {
            // Avoid subplanning by appending a root $and with a benign
            // conjunct.
            filter = {$and: [filter, {[clusterKeyFieldName]: {$not: {$type: "symbol"}}}]};
        }

        // Constructs a query with the given arguments.
        const makeQuery = () => {
            let query = coll.find(filter);
            if (collation) {
                query = query.collation(collation);
            }
            if (min) {
                query = query.min(min);
            }
            if (max) {
                query = query.max(max);
            }
            // min/max requires hint.
            if (min || max) {
                query = query.hint({[clusterKeyFieldName]: 1});
            }
            return query;
        };

        const forward = makeQuery()
            .sort({[clusterKeyFieldName]: 1})
            .toArray();
        assert.eq(
            forward.map((d) => d[clusterKeyFieldName]),
            expectedIds,
        );
        // TODO SERVER-133667 remove 'featureFlagClusteredCollScanMultiRange'.
        const multiRangeEnabled = FeatureFlagUtil.isPresentAndEnabledOnAllNodes(
            coll.getDB(),
            "ClusteredCollScanMultiRange",
        );
        // Test both directions' number of seeks.
        for (const [sortDir, expectedSeeks, expectedDocsExaminedClassic] of [
            [1, classicExpectedSeeksForward, expectedDocsExaminedClassicForward],
            // min/max does not support backwards scans.
            ...(min || max
                ? []
                : [[-1, classicExpectedSeeksBackward, expectedDocsExaminedClassicBackward]]),
        ]) {
            const explain = makeQuery()
                .sort({[clusterKeyFieldName]: sortDir})
                .explain("executionStats");
            const executionStages = getExecutionStages(explain);

            // Verify recordIdRanges/minRecord/maxRecord for the winning plan in the queryPlanner section.
            // These checks require a CLUSTERED_IXSCAN, which the legacy (flag-off) path may not
            // produce for $or queries (it collapses to an (unbounded) COLLSCAN).
            // Correctness is covered by expectedIds and assertForwardBackwardConsistency below.
            const winningPlan = getWinningPlanFromExplain(explain);
            const qpScan = getPlanStage(winningPlan, "CLUSTERED_IXSCAN");

            if (multiRangeEnabled) {
                assert(qpScan, "Expected CLUSTERED_IXSCAN in queryPlanner winningPlan");

                // Skip recordIdRanges/minRecord/maxRecord checks if expectedRanges is not provided
                if (typeof expectedRanges !== "undefined") {
                    // The explain output contains no recordIdRanges field if there is a single range.
                    if (expectedRanges.length != 1) {
                        assert.docEq(
                            expectedRanges,
                            qpScan.recordIdRanges,
                            "Unexpected recordIdRanges in queryPlanner",
                        );
                    } else {
                        assert(!("recordIdRanges" in qpScan));
                    }
                    // The output always has the minRecord and maxRecord fields.
                    assert.eq(
                        qpScan.minRecord,
                        expectedRanges.length > 0 ? expectedRanges[0].min : null,
                    );
                    assert.eq(
                        qpScan.maxRecord,
                        expectedRanges.length > 0
                            ? expectedRanges[expectedRanges.length - 1].max
                            : null,
                    );
                }
            }

            const clusteredIxscan = getPlanStage(executionStages[0], "CLUSTERED_IXSCAN");
            if (triviallyEmpty) {
                if (clusteredIxscan) {
                    // Finding a CLUSTERED_IXSCAN in the executionStats section means it was the
                    // classic engine that ran this query. The classic engine creates an empty
                    // CLUSTERED_IXSCAN with the feature flag on and one with inverted bounds
                    // (max < min) if it is off.
                    if (multiRangeEnabled) {
                        assert.eq(
                            clusteredIxscan.recordIdRanges.length,
                            0,
                            "Expected empty CLUSTERED_IXSCAN for trivially empty query.",
                        );
                        assert.eq(clusteredIxscan.seeks, expectedSeeks, clusteredIxscan);
                        assertDocsExamined(
                            explain,
                            expectedDocsExaminedClassic,
                            expectedDocsExaminedSbe,
                        );
                    }
                    // The flag-off case is covered by the minRecord/maxRecord checks.
                } else {
                    // We have a CLUSTERED_IXSCAN in the winningPlan, but not in the
                    // executionStats section. Then the query must have ran with SBE.
                    if (multiRangeEnabled) {
                        // SBE creates a no-op plan (limit+coscan) with no scan stage.
                        const limitStage = getPlanStage(executionStages[0], "limit");
                        assert(
                            limitStage,
                            "Expected a limit stage in the no-op plan for empty range intersection.",
                        );
                        assert.eq(
                            limitStage.limit,
                            0,
                            "Expected limit(0) as the no-op for empty range intersection.",
                        );
                        assert(
                            getPlanStage(executionStages[0], "coscan"),
                            "Expected a coscan stage in the no-op plan for empty range intersection.",
                        );
                    } else {
                        // With the feature flag off, SBE generates a regular scan stage.
                        assert(
                            getPlanStage(executionStages[0], "scan"),
                            "Expected a scan stage but got " + tojson(executionStages[0]),
                        );
                    }
                }
            } else if (multiRangeEnabled) {
                // Check execution stats only with the feature flag enabled
                if (!clusteredIxscan) {
                    // It is just called "scan" for SBE - make sure there is one.
                    const sbeScanStage = getPlanStage(executionStages[0], "scan");
                    assert(
                        sbeScanStage,
                        "Expected to find either a CLUSTERED_IXSCAN or a scan but got " +
                            JSON.stringify(executionStages[0], null, 4),
                    );
                    // We cannot verify the number of seeks for SBE scans because they don't
                    // report it as part of their stats. We can verify nReturned/nReads though.
                    assert.eq(sbeScanStage.nReturned, expectedDocsExaminedSbe, sbeScanStage);
                    assert.eq(sbeScanStage.numReads, expectedDocsExaminedSbe, sbeScanStage);
                } else {
                    // Check the seek and docs examined count for the classic engine.
                    assert.eq(clusteredIxscan.seeks, expectedSeeks, clusteredIxscan);
                    assertDocsExamined(
                        explain,
                        expectedDocsExaminedClassic,
                        expectedDocsExaminedSbe,
                    );
                }
            }
        }

        // min/max does not support backwards scans.
        if (!min && !max) {
            assertForwardBackwardConsistency(filter, "assertMultiRangeResults");
        }
    }

    // Tests forward and backward scans over multiple non-contiguous ranges.
    function testMultiRangeForwardBackward() {
        initAndPopulate(coll, clusterKey);

        // Ranges: [1,5], (10,15), [17,17], [20,25)
        // Not in sorted order
        const filter = {
            $or: [
                {[clusterKeyFieldName]: {$eq: 17}},
                {[clusterKeyFieldName]: {$gte: 1, $lte: 5}},
                {[clusterKeyFieldName]: {$gte: 2, $lte: 5}}, // is subsumed by the previous one
                {[clusterKeyFieldName]: {$gte: 20, $lt: 25}},
                {[clusterKeyFieldName]: {$gt: 10, $lt: 15}},
            ],
        };
        const expectedIds = [1, 2, 3, 4, 5, 11, 12, 13, 14, 17, 20, 21, 22, 23, 24];
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 4,
            classicExpectedSeeksBackward: 4,
            expectedDocsExaminedClassic: 19,
            expectedDocsExaminedSbe: 15,
            expectedRanges: [
                {min: 1, minInclusive: true, max: 5, maxInclusive: true},
                {min: 10, minInclusive: false, max: 15, maxInclusive: false},
                {min: 17, minInclusive: true, max: 17, maxInclusive: true},
                {min: 20, minInclusive: true, max: 25, maxInclusive: false},
            ],
        });
    }

    // Tests that ranges that fall between gaps in the collection data return no documents.
    function testMultiRangeSomeRangesEmpty() {
        // [clusterKeyFieldName] 1..10 and 20..30
        const ids = [
            ...Array.from({length: 10}, (_, i) => i + 1),
            ...Array.from({length: 11}, (_, i) => i + 20),
        ];
        initAndPopulate(coll, clusterKey, ids);

        // [1,5] has data; [12,14] and [16, 19] has no data; [22,28] has data; [35, 40] has no data.
        const filter = {
            $or: [
                {[clusterKeyFieldName]: {$gte: 1, $lte: 5}},
                {[clusterKeyFieldName]: {$gte: 12, $lte: 14}},
                {[clusterKeyFieldName]: {$gte: 16, $lte: 19}},
                {[clusterKeyFieldName]: {$gte: 22, $lte: 28}},
                {[clusterKeyFieldName]: {$gte: 35, $lte: 40}},
            ],
        };
        const expectedIds = [1, 2, 3, 4, 5, 22, 23, 24, 25, 26, 27, 28];
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 4,
            classicExpectedSeeksBackward: 4,
            expectedDocsExaminedClassic: 16,
            expectedDocsExaminedSbe: 12,
            expectedRanges: [
                {min: 1, minInclusive: true, max: 5, maxInclusive: true},
                {min: 12, minInclusive: true, max: 14, maxInclusive: true},
                {min: 16, minInclusive: true, max: 19, maxInclusive: true},
                {min: 22, minInclusive: true, max: 28, maxInclusive: true},
                {min: 35, minInclusive: true, max: 40, maxInclusive: true},
            ],
        });
    }

    function testMultiRangeCursorSeeksForYou() {
        // [clusterKeyFieldName] 1..10 and 20..30
        const ids = [
            ...Array.from({length: 10}, (_, i) => i + 1),
            ...Array.from({length: 11}, (_, i) => i + 20),
        ];
        initAndPopulate(coll, clusterKey, ids);

        // [1, 10] and [20, 30]
        const filter = {
            $or: [
                {[clusterKeyFieldName]: {$gte: 1, $lte: 10}},
                {[clusterKeyFieldName]: {$gte: 20, $lte: 30}},
            ],
        };
        const expectedIds = ids;
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 1,
            classicExpectedSeeksBackward: 1,
            expectedDocsExaminedClassic: 22,
            expectedDocsExaminedSbe: 21,
            expectedRanges: [
                {min: 1, minInclusive: true, max: 10, maxInclusive: true},
                {min: 20, minInclusive: true, max: 30, maxInclusive: true},
            ],
        });
    }

    // Tests adjacent ranges where the shared boundary point is excluded by both.
    function testMultiRangeExclusiveJunction() {
        initAndPopulate(coll, clusterKey);

        // [1,5) and (5,10] — value 5 is excluded from both ranges
        const filter = {
            $or: [
                {[clusterKeyFieldName]: {$gte: 1, $lt: 5}},
                {[clusterKeyFieldName]: {$gt: 5, $lte: 10}},
            ],
        };
        // 1,2,3,4 then 6,7,8,9,10 — 5 must not appear
        const expectedIds = [1, 2, 3, 4, 6, 7, 8, 9, 10];
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 2,
            classicExpectedSeeksBackward: 2,
            expectedDocsExaminedClassic: 11,
            expectedDocsExaminedSbe: 9,
            expectedRanges: [
                {min: 1, minInclusive: true, max: 5, maxInclusive: false},
                {min: 5, minInclusive: false, max: 10, maxInclusive: true},
            ],
        });
    }

    // Tests multi-range scan combined with an additional filter predicate.
    function testMultiRangeWithFilter() {
        initAndPopulate(coll, clusterKey);

        // Ranges [1,10] and [15,25], keeping only even values
        const filter = {
            $and: [
                {
                    $or: [
                        {[clusterKeyFieldName]: {$gte: 1, $lte: 10}},
                        {[clusterKeyFieldName]: {$gte: 15, $lte: 25}},
                    ],
                },
                // The 'a' field is populated as -i (see initAndPopulate),
                // so this equivalently filters on even _id.
                {a: {$type: "number", $mod: [2, 0]}},
            ],
        };
        // Even values in [1,10]: 2,4,6,8,10; in [15,25]: 16,18,20,22,24
        const expectedIds = [2, 4, 6, 8, 10, 16, 18, 20, 22, 24];
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 2,
            classicExpectedSeeksBackward: 2,
            expectedDocsExaminedClassic: 23,
            expectedDocsExaminedSbe: 21,
            expectedRanges: [
                {min: 1, minInclusive: true, max: 10, maxInclusive: true},
                {min: 15, minInclusive: true, max: 25, maxInclusive: true},
            ],
        });

        // Verify that the non-cluster-key filter is applied by the scan stage.
        // Use explain to inspect the execution stage.
        if (
            FeatureFlagUtil.isPresentAndEnabledOnAllNodes(
                coll.getDB(),
                "ClusteredCollScanMultiRange",
            )
        ) {
            const explain = coll
                .explain("executionStats")
                .find(filter)
                .sort({[clusterKeyFieldName]: 1})
                .finish();
            const executionStages = getExecutionStages(explain);
            const clusteredIxscan = getPlanStage(executionStages[0], "CLUSTERED_IXSCAN");
            if (clusteredIxscan) {
                // Classic: the filter should be present on the CLUSTERED_IXSCAN stage.
                assert(clusteredIxscan.filter, "Expected filter on CLUSTERED_IXSCAN", {
                    clusteredIxscan,
                });
                assert.eq(clusteredIxscan.nReturned, expectedIds.length, {
                    clusteredIxscan,
                });
            } else {
                // Must be an SBE scan. Then we do not check the filter.
                assert(
                    getPlanStage(executionStages[0], "scan"),
                    "Expected either a classic or an SBE collscan.",
                    executionStages,
                );
            }
        }
    }

    // Tests ranges where the first range has no lower bound and the last has no upper bound.
    function testMultiRangeUnboundedEnds() {
        initAndPopulate(coll, clusterKey);

        // (-∞, 5) and (10, +∞)
        const filter = {
            // We cannot naively use $lt/$gt here, because they implicitly bound the ranges by NaN/Inf.
            // So we wrap them inside an $expr.
            $expr: {
                $or: [
                    {$lt: [`$${clusterKeyFieldName}`, 5]},
                    {$gt: [`$${clusterKeyFieldName}`, 10]},
                ],
            },
        };
        const expectedIds = [
            null,
            ...Array.from({length: batchSize}, (_, i) => i).filter((idx) => idx < 5 || idx > 10),
            "foo",
        ];
        // Don't forget the null and string ids.
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 1,
            classicExpectedSeeksBackward: 1,
            expectedDocsExaminedClassic: batchSize + 2 - 5,
            expectedDocsExaminedSbe: batchSize + 2 - 6,
            expectedRanges: [
                {max: 5, maxInclusive: false},
                {min: 10, minInclusive: false},
            ],
        });
    }

    // Tests that forward and backward scans can produce a different number of seeks when
    // the outer bounds are unbounded on only one side. The outerBounds() are [0, +∞), so
    // forward does an initial seek to 0 (1 seek) plus a seek to 5 (1 seek) = 2 seeks total,
    // while backward has no initial seek (unbounded max) and only seeks to 0 = 1 seek total.
    function testMultiRangeAsymmetricSeeks() {
        initAndPopulate(coll, clusterKey);

        // [0,0] and [5, +∞). Use $expr for the $gte to avoid type bracketing (which would
        // bound the range to [5, maxForType(numberInt)] instead of [5, +∞)).
        const filter = {
            $or: [
                {[clusterKeyFieldName]: {$eq: 0}},
                {$expr: {$gte: [`$${clusterKeyFieldName}`, 5]}},
            ],
        };
        // $expr with $gte matches all types ≥ 5, including "foo" (string > number in BSON).
        const expectedIds = [0, ...Array.from({length: batchSize - 5}, (_, i) => i + 5), "foo"];
        // outerBounds() = [0, +∞): forward seeks to min=0 (1 seek), then after overshooting
        // past 0, seeks to 5 (1 seek). Backward has no initial seek (max is absent), reads
        // from end, then seeks to 0 (1 seek). Forward=2 seeks, backward=1 seek.
        //
        // Docs examined (classic counts overshoot, SBE does not):
        // Forward: 0 (match), 1 (overshoot from [0,0]), 5..99,"foo" (96 matches), EOF (no
        //   overshoot) = 1 + 1 + 96 = 98 = batchSize + 1 - 3.
        // Backward: "foo",99..5 (96 matches), 4 (overshoot from [5,+∞)), 0 (match), null
        //   (overshoot from [0,0]) = 96 + 1 + 1 + 1 = 99 = batchSize - 3 + 2.
        // SBE: 0, 5..99, "foo" (97 matches, excludes overshoots) = batchSize + 1 - 4.
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 2,
            classicExpectedSeeksBackward: 1,
            expectedDocsExaminedClassicForward: batchSize + 1 - 3,
            expectedDocsExaminedClassicBackward: batchSize - 3 + 2,
            expectedDocsExaminedSbe: batchSize + 1 - 4,
            expectedRanges: [
                {min: 0, minInclusive: true, max: 0, maxInclusive: true},
                {min: 5, minInclusive: true},
            ],
        });
    }

    // Test .min()/.max() combined with a multi-range $in filter on the cluster key.
    function testMultiRangeWithMinMax() {
        initAndPopulate(coll, clusterKey);

        const expectedIds = [5, 7];
        // The $in produces point ranges; .min()/.max() further bounds the scan.
        // $in: [2, 5, 7, 9] ∩ min=5 (inclusive) / max=9 (exclusive) = [5,5], [7,7].
        const filter = {[clusterKeyFieldName]: {$in: [2, 5, 7, 9]}};

        assertMultiRangeResults({
            filter: filter,
            min: {[clusterKeyFieldName]: 5},
            max: {[clusterKeyFieldName]: 9},
            expectedIds,
            classicExpectedSeeksForward: 2,
            classicExpectedSeeksBackward: 2,
            expectedDocsExaminedClassicForward: 4,
            expectedDocsExaminedClassicBackward: 4,
            expectedDocsExaminedSbe: 2,
            expectedRanges: [
                {min: 5, minInclusive: true, max: 5, maxInclusive: true},
                {min: 7, minInclusive: true, max: 7, maxInclusive: true},
            ],
        });
    }

    // Tests that $and of two overlapping cluster-key ranges computes their intersection via
    // RecordIdRangeList::intersect, scanning only the narrower overlapping region.
    function testAndIntersectsRanges() {
        initAndPopulate(coll, clusterKey);

        // [5,20] ∩ [10,25] = [10,20]
        const filter = {
            $and: [
                {[clusterKeyFieldName]: {$gte: 5, $lte: 20}},
                {[clusterKeyFieldName]: {$gte: 10, $lte: 25}},
            ],
        };
        const expectedIds = Array.from({length: 11}, (_, i) => i + 10); // 10..20
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 1,
            classicExpectedSeeksBackward: 1,
            expectedDocsExaminedClassic: 12,
            expectedDocsExaminedSbe: 11,
            expectedRanges: [{min: 10, minInclusive: true, max: 20, maxInclusive: true}],
        });
    }

    // Tests that $and of two disjoint cluster-key ranges produces an empty result without any seeks.
    function testAndDisjointRangesEmpty() {
        initAndPopulate(coll, clusterKey);

        // [5,10] ∩ [15,20] = ∅
        const filter = {
            $and: [
                {[clusterKeyFieldName]: {$gte: 5, $lte: 10}},
                {[clusterKeyFieldName]: {$gte: 15, $lte: 20}},
            ],
        };
        const expectedIds = [];
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 0,
            classicExpectedSeeksBackward: 0,
            expectedDocsExaminedClassic: 0,
            expectedDocsExaminedSbe: 0,
            triviallyEmpty: true,
            expectedRanges: [],
        });

        assertForwardBackwardConsistency(filter, "testAndDisjointRangesEmpty");
    }

    // Tests that the bounds of an $and with a non-range branch ($mod) is NOT fully exact: the scan is
    // still bounded by the range branches but the $mod filter must be applied post-scan.
    function testAndOneBranchNotExact() {
        initAndPopulate(coll, clusterKey);

        // Scan is bounded to [5,20] by the range sub-expression, but $mod cannot be expressed
        // as a scan bound, so it is retained.
        const filter = {
            $and: [
                {[clusterKeyFieldName]: {$gte: 5, $lte: 20}},
                {[clusterKeyFieldName]: {$mod: [2, 0]}},
            ],
        };
        // Even numbers in [5,20]: 6,8,10,12,14,16,18,20
        const expectedIds = Array.from({length: 8}, (_, i) => (i + 3) * 2);
        // Range [5,20]: 16 docs in range + 1 Classic overshoot at 21.
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 1,
            classicExpectedSeeksBackward: 1,
            expectedDocsExaminedClassic: 17,
            expectedDocsExaminedSbe: 16,
            expectedRanges: [{min: 5, minInclusive: true, max: 20, maxInclusive: true}],
        });
    }

    // Tests that $or where one branch does not have exact bounds keeps the filter intact and bounds the
    // scan only to the union of the (loose) ranges.
    function testOrOneBranchNotExact() {
        initAndPopulate(coll, clusterKey);

        // Branch 0 is an $and whose range sub-expression ([0,10]) has exact bounds, but whose
        // $mod predicate is NOT (it cannot be expressed as a scan bound). Therefore branch 0,
        // and the $or as a whole, does NOT have exact bounds. The full filter must remain so that odd
        // numbers in [0,10] are correctly excluded from the results.
        // Branch 1 has the exact bound [15,25].
        // Both branches are on the cluster key, so the planner uses a single CLUSTERED_IXSCAN
        // bounded to [0,10] ∪ [15,25] — no subplanning is triggered.
        const filter = {
            $or: [
                {
                    $and: [
                        {[clusterKeyFieldName]: {$gte: 0, $lte: 10}},
                        {[clusterKeyFieldName]: {$mod: [2, 0]}},
                    ],
                },
                {[clusterKeyFieldName]: {$gte: 15, $lte: 25}},
            ],
        };
        // Even ids in [0,10] + all ids in [15,25]
        const expectedIds = [
            ...Array.from({length: 6}, (_, i) => i * 2), // 0,2,4,6,8,10
            ...Array.from({length: 11}, (_, i) => i + 15), // 15..25
        ];
        // 2 seeks (one per range). Classic: (11 docs + 1 overshoot) × 2 ranges = 24 docs.
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 2,
            classicExpectedSeeksBackward: 2,
            expectedDocsExaminedClassic: 24,
            expectedDocsExaminedSbe: 22,
            expectedRanges: [
                {min: 0, minInclusive: true, max: 10, maxInclusive: true},
                {min: 15, minInclusive: true, max: 25, maxInclusive: true},
            ],
        });
    }

    // Tests that a $not predicate is preserved as a residual filter. $not range
    // complementation is not yet implemented for clustered indices. Additionally,
    // applySimplifications intentionally does not descend into $not nodes: a trivially-true
    // child of $not would make the $not trivially false, the opposite of what nulling implies.
    function testNotPreservesFilter() {
        initAndPopulate(coll, clusterKey);

        const filter = {
            $and: [
                {[clusterKeyFieldName]: {$gte: 0, $lte: 10}},
                {[clusterKeyFieldName]: {$not: {$mod: [2, 0]}}},
            ],
        };
        // Odd numbers in [0,10]: 1,3,5,7,9
        const expectedIds = [1, 3, 5, 7, 9];
        // 1 seek covering [0,10]. Classic: 11 docs in [0,10] + 1 overshoot = 12 docs examined.
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 1,
            classicExpectedSeeksBackward: 1,
            expectedDocsExaminedClassic: 12,
            expectedDocsExaminedSbe: 11,
            expectedRanges: [{min: 0, minInclusive: true, max: 10, maxInclusive: true}],
        });
    }

    // Tests that a $nor query produces correct results with a clustered collection bounded scan.
    // $nor/$not range complementation is not yet implemented for clustered indices, so the $nor
    // is kept as a residual filter. applySimplifications intentionally does not descend into
    // $nor nodes: a trivially-true child of $nor makes the $nor trivially FALSE — the opposite
    // of what nulling it would imply.
    function testNorPreservesFilter() {
        initAndPopulate(coll, clusterKey);

        const filter = {
            $and: [
                {[clusterKeyFieldName]: {$gte: 0, $lte: 30}},
                {$nor: [{[clusterKeyFieldName]: {$gte: 10, $lte: 20}}]},
            ],
        };
        // All ids in [0,30] except [10,20]: [0..9] ∪ [21..30]
        const expectedIds = [
            ...Array.from({length: 10}, (_, i) => i), // 0..9
            ...Array.from({length: 10}, (_, i) => i + 21), // 21..30
        ];
        // $nor/$not range complementation is not yet implemented, so the scan uses the outer
        // [0,30] bounds and the $nor is kept as a residual filter.
        // Classic: 31 docs in [0,30] + 1 overshoot = 32 examined.
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 1,
            classicExpectedSeeksBackward: 1,
            expectedDocsExaminedClassic: 32,
            expectedDocsExaminedSbe: 31,
            expectedRanges: [{min: 0, minInclusive: true, max: 30, maxInclusive: true}],
        });
    }

    // Tests that a predicate with non-exact bounds ($mod) as a direct child of $or (not nested inside
    // an $and) is preserved as a residual filter. The outer {$gte:0,$lte:25} bounds the scan;
    // neither $or branch is redundant for the full scan range, so the $or remains.
    function testOrNonExactDirectChild() {
        initAndPopulate(coll, clusterKey);

        const filter = {
            $and: [
                {
                    $or: [
                        {[clusterKeyFieldName]: {$mod: [2, 0]}},
                        {[clusterKeyFieldName]: {$gte: 15, $lte: 25}},
                    ],
                },
                {[clusterKeyFieldName]: {$gte: 0, $lte: 25}},
            ],
        };
        // (id % 2 == 0) OR (15 <= id <= 25), restricted to [0,25]:
        //   even ids in [0,14] + all ids in [15,25]
        const expectedIds = [
            ...Array.from({length: 8}, (_, i) => i * 2), // 0,2,4,6,8,10,12,14
            ...Array.from({length: 11}, (_, i) => i + 15), // 15..25
        ];
        // 1 seek covering [0,25]. Classic: 26 docs in [0,25] + 1 overshoot = 27 docs examined.
        assertMultiRangeResults({
            filter,
            expectedIds,
            classicExpectedSeeksForward: 1,
            classicExpectedSeeksBackward: 1,
            expectedDocsExaminedClassic: 27,
            expectedDocsExaminedSbe: 26,
            expectedRanges: [{min: 0, minInclusive: true, max: 25, maxInclusive: true}],
        });
    }

    function testCollationCaseInsensitiveSimple() {
        const db = coll.getDB();
        assertDropCollection(db, coll.getName());
        assert.commandWorked(
            db.createCollection(coll.getName(), {
                clusteredIndex: {key: {[clusterKeyFieldName]: 1}, unique: true},
                collation: {locale: "en", strength: 1},
            }),
        );

        assert.commandWorked(coll.insertOne({[clusterKeyFieldName]: "a"}));

        const filter = {[clusterKeyFieldName]: {$in: ["A", "B"]}};
        // This scans the entire range of strings.
        // Explicitly override the collation of the collection to force collation incompatibility.
        assertMultiRangeResults({
            filter: filter,
            expectedIds: [],
            classicExpectedSeeksForward: 1,
            classicExpectedSeeksBackward: 1,
            expectedDocsExaminedClassic: 1,
            expectedDocsExaminedSbe: 1,
            collation: {locale: "simple"},
            expectedRanges: [{min: "", minInclusive: true, max: {}, maxInclusive: false}],
        });
    }

    function testCollationCaseInsensitiveCompatible() {
        const db = coll.getDB();
        assertDropCollection(db, coll.getName());
        assert.commandWorked(
            db.createCollection(coll.getName(), {
                clusteredIndex: {key: {[clusterKeyFieldName]: 1}, unique: true},
                collation: {locale: "en", strength: 1},
            }),
        );

        assert.commandWorked(
            coll.insertMany(
                ["a", "b", "c", "d", "z"].map((id) => {
                    return {[clusterKeyFieldName]: id};
                }),
            ),
        );

        const filter = {[clusterKeyFieldName]: {$in: ["A", "D"]}};

        // No explicit collation: uses the collection's collation, so we have compatible collations.
        // With case-insensitive collation (strength: 1), "A" matches "a" and "D" matches "d".
        // $in: ["A", "D"] produces two point ranges with a gap ("b", "c" between them), proving
        // the stage seeks rather than scanning the entire [a, d] interval.
        // Classic forward: 2 seeks (to "a" and "d"), 4 docs examined (a, b overshoot, d, z overshoot).
        // Classic backward: 2 seeks (to "d" and "a"), 3 docs examined (d, c overshoot, a, then
        //   EOF — no overshoot). 2 seeks, 3 docs.
        // SBE: 2 docs examined (a and d, no overshoot).
        assertMultiRangeResults({
            filter,
            expectedIds: ["a", "d"],
            classicExpectedSeeksForward: 2,
            classicExpectedSeeksBackward: 2,
            expectedDocsExaminedClassicForward: 4,
            expectedDocsExaminedClassicBackward: 3,
            expectedDocsExaminedSbe: 2,
            // Omit expectedRanges here - it contains the collated strings.
        });
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
        testBackwardBoundedScans();

        testMultiRangeForwardBackward();
        testMultiRangeSomeRangesEmpty();
        testMultiRangeExclusiveJunction();
        testMultiRangeCursorSeeksForYou();
        testMultiRangeWithFilter();
        testMultiRangeUnboundedEnds();
        testMultiRangeAsymmetricSeeks();
        testMultiRangeWithMinMax();

        testAndIntersectsRanges();
        testAndDisjointRangesEmpty();

        testAndOneBranchNotExact();
        testOrOneBranchNotExact();
        testNotPreservesFilter();
        testNorPreservesFilter();
        testOrNonExactDirectChild();

        testCollationCaseInsensitiveSimple();
        testCollationCaseInsensitiveCompatible();
    }

    return testBoundedScans(coll, clusterKey);
};
