/**
 * Tests the metrics.query.sort.* ServerStatus counters.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

function getSortMetrics() {
    return db.serverStatus().metrics.query.sort;
}

describe("metrics.query.sort", function () {
    // SortedFileWriter flushes its buffer to the spill file every kSortedFileBufferSize (64KB, see
    // file_based_spiller.h), so a memory limit bigger than that makes one logical spill produce
    // more than one flush.
    const kMemoryLimitBytes = 150 * 1024;
    // Under the memory limit alone, but two of them exceed it together, so both accumulate into
    // one spill instead of each spilling on its own.
    const kDocPayloadBytes = 100 * 1024;
    let originalMemoryLimitBytes;

    assert.lt(kDocPayloadBytes, kMemoryLimitBytes);
    assert.gt(2 * kDocPayloadBytes, kMemoryLimitBytes);

    before(function () {
        originalMemoryLimitBytes = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1}),
        ).internalQueryMaxBlockingSortMemoryUsageBytes;
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryMaxBlockingSortMemoryUsageBytes: kMemoryLimitBytes,
            }),
        );

        this.coll = db.spill_to_disk;
        assert(this.coll.drop());

        const bigStr = ",".repeat(kDocPayloadBytes);
        // These two entries form one logical spill, with each exceeding the 64KB writer buffer.
        for (let i = 0; i < 2; i++) {
            assert.commandWorked(
                this.coll.insert({_id: i, bigStr: i + bigStr, random: Math.random()}),
            );
        }
        assert.gt(this.coll.stats().size, kMemoryLimitBytes);
    });

    after(function () {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryMaxBlockingSortMemoryUsageBytes: originalMemoryLimitBytes,
            }),
        );
    });

    it("counts the keys and the bytes sorted", function () {
        const metricsBefore = getSortMetrics();
        assert.eq(this.coll.aggregate([{$sort: {random: 1}}]).itcount(), 2);
        const metricsAfter = getSortMetrics();

        assert.gt(
            metricsAfter.totalKeysSorted,
            metricsBefore.totalKeysSorted,
            "Expect metric query.sort.totalKeysSorted to increment after pipeline",
            {metricsBefore, metricsAfter},
        );
        assert.gt(
            metricsAfter.totalBytesSorted,
            metricsBefore.totalBytesSorted,
            "Expect metric query.sort.totalBytesSorted to increment after pipeline",
            {metricsBefore, metricsAfter},
        );
    });

    it("counts a sort that spills to disk", function () {
        const metricsBefore = getSortMetrics();
        assert.eq(this.coll.aggregate([{$sort: {random: 1}}]).itcount(), 2);
        const metricsAfter = getSortMetrics();

        assert.gt(
            metricsAfter.spillToDisk,
            metricsBefore.spillToDisk,
            "Expect metric query.sort.spillToDisk to increment after pipeline",
            {metricsBefore, metricsAfter},
        );
    });

    // A single logical spill of the in-memory buffer reaches the spill file as more than one
    // kSortedFileBufferSize chunk write. The counter must track logical spills rather than those
    // chunk writes, so cross-check it against the spill count the same query reports through
    // explain.
    it("counts one spill per logical spill", function () {
        const spillsBefore = getSortMetrics().spillToDisk;
        const explain = this.coll.explain("executionStats").aggregate([{$sort: {random: 1}}]);
        const spillsAfter = getSortMetrics().spillToDisk;

        // "SORT" is the classic sort stage, "sort" its SBE counterpart.
        const sortStages = [
            ...getAggPlanStages(explain, "SORT"),
            ...getAggPlanStages(explain, "sort"),
        ];
        assert.eq(
            sortStages.length,
            1,
            "Expect explain of the pipeline to report exactly one sort stage",
            {explain},
        );
        assert.gt(sortStages[0].spills, 0, "Expect the sort stage to have spilled to disk", {
            explain,
        });

        assert.eq(
            spillsAfter - spillsBefore,
            sortStages[0].spills,
            "Expect metric query.sort.spillToDisk to increment once per logical spill, matching " +
                "the number of spills the sort stage reports in explain",
            {explain},
        );
    });
});
