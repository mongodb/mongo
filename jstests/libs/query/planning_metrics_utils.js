/**
 * Shared utilities for asserting CBR and multiPlanner serverStatus metrics in JS tests.
 */

/**
 * Returns the total number of recorded observations across all histogram buckets.
 */
export function sumHistogramBucketCounts(histogram) {
    let sum = 0;
    for (const [key, bucket] of Object.entries(histogram)) {
        if (bucket.hasOwnProperty("count")) {
            sum += bucket.count;
        }
    }
    return sum;
}

/**
 * Returns the bounds of a histogram.
 */
export function histogramPowBounds(sz, base, rate) {
    const bounds = [];
    for (let i = 0; i < sz; i++, base *= rate) {
        bounds.push(base);
    }
    return bounds;
}

/**
 * Verifies a histogram is a well formed array of {lowerBound, count} buckets.
 */
export function assertHistogramBoundaries(histogram, expectedBounds) {
    assert(Array.isArray(histogram), "histogram should be an array", {histogram});
    assert.eq(
        histogram.length,
        expectedBounds.length + 1,
        "histogram should have one bucket per bound plus the underflow bucket",
        {histogram},
    );
    assert.eq(Number(histogram[0].lowerBound), 0, "first bucket lowerBound should be 0", {
        histogram,
    });
    for (let i = 0; i < expectedBounds.length; i++) {
        assert.eq(
            Number(histogram[i + 1].lowerBound),
            expectedBounds[i],
            `bucket ${i + 1} lowerBound should match expected edge`,
            {histogram, expectedBounds},
        );
    }
}

export function getCBRMetrics(db) {
    return db.serverStatus().metrics.query.cbr;
}

export function getMultiPlannerMetrics(db) {
    return db.serverStatus().metrics.query.multiPlanner;
}

export function getPlanningMetrics(db) {
    return db.serverStatus().metrics.query.planning;
}

export function getAnalyzeMetrics(db) {
    return db.serverStatus().metrics.query.analyze.sample;
}

export function getPersistentSampleMetrics(db) {
    return db.serverStatus().metrics.query.sampling.persistentSample;
}

export function getAnalyzeCommandMetrics(db) {
    return db.serverStatus().metrics.commands.analyze;
}

export function assertCBRDidNotRun(cbrBefore, cbrAfter) {
    assert.eq(
        cbrAfter.count,
        cbrBefore.count,
        `cbr.count should not change when MP wins. Previous value: ${cbrBefore.count} Current value: ${cbrAfter.count}`,
    );
    assert.eq(
        cbrAfter.choseWinningPlan,
        cbrBefore.choseWinningPlan,
        `cbr.choseWinningPlan should not change when MP wins. Previous value: ${cbrBefore.choseWinningPlan} Current value: ${cbrAfter.choseWinningPlan}`,
    );
}

export function assertMPChoseWinner(mpBefore, mpAfter) {
    assert.eq(
        mpAfter.classicCount,
        mpBefore.classicCount + 1,
        `multiPlanner.classicCount should increase by 1. Previous value: ${mpBefore.classicCount} Current value: ${mpAfter.classicCount}`,
    );
    assert.eq(
        mpAfter.choseWinningPlan,
        mpBefore.choseWinningPlan + 1,
        `multiPlanner.choseWinningPlan should increase by 1 when MP wins. Previous value: ${mpBefore.choseWinningPlan} Current value: ${mpAfter.choseWinningPlan}`,
    );
}

export function assertCBRChoseWinner(cbrBefore, cbrAfter) {
    assert.eq(
        cbrAfter.count,
        cbrBefore.count + 1,
        `cbr.count should increase by 1 when CBR wins. Previous value: ${cbrBefore.count} Current value: ${cbrAfter.count}`,
    );
    assert.eq(
        cbrAfter.choseWinningPlan,
        cbrBefore.choseWinningPlan + 1,
        `cbr.choseWinningPlan should increase by 1 when CBR wins. Previous value: ${cbrBefore.choseWinningPlan} Current value: ${cbrAfter.choseWinningPlan}`,
    );
}

export function assertOneHistogramObservation(mpBefore, mpAfter) {
    const worksBefore = sumHistogramBucketCounts(mpBefore.histograms.classicWorks);
    const worksAfter = sumHistogramBucketCounts(mpAfter.histograms.classicWorks);
    assert.eq(
        worksAfter,
        worksBefore + 1,
        `histograms.classicWorks should gain one observation. Previous value: ${worksBefore} Current value: ${worksAfter}`,
    );
    const microsBefore = sumHistogramBucketCounts(mpBefore.histograms.classicMicros);
    const microsAfter = sumHistogramBucketCounts(mpAfter.histograms.classicMicros);
    assert.eq(
        microsAfter,
        microsBefore + 1,
        `histograms.classicMicros should gain one observation. Previous value: ${microsBefore} Current value: ${microsAfter}`,
    );
    const numPlansBefore = sumHistogramBucketCounts(mpBefore.histograms.classicNumPlans);
    const numPlansAfter = sumHistogramBucketCounts(mpAfter.histograms.classicNumPlans);
    assert.eq(
        numPlansAfter,
        numPlansBefore + 1,
        `histograms.classicNumPlans should gain one observation. Previous value: ${numPlansBefore} Current value: ${numPlansAfter}`,
    );
}

export function assertOnePlanningInvocation(planningBefore, planningAfter) {
    assert.eq(
        planningAfter.invocationCount,
        planningBefore.invocationCount + 1,
        `planning.invocationCount should increase by 1. Previous value: ${planningBefore.invocationCount} Current value: ${planningAfter.invocationCount}`,
    );
}
