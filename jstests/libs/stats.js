// Utility functions for obtaining and diffing top and latency histogram statistics.

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 * Returns the latency histograms for the given collection.
 */
export function getHistogramStats(coll) {
    return coll.latencyStats().next().latencyStats;
}

/**
 * Returns the differences in read, write, and command counts between two histograms.
 */
export function diffHistogram(thisHistogram, lastHistogram) {
    return {
        reads: thisHistogram.reads.ops - lastHistogram.reads.ops,
        writes: thisHistogram.writes.ops - lastHistogram.writes.ops,
        commands: thisHistogram.commands.ops - lastHistogram.commands.ops
    };
}

/**
 * Asserts that the difference of histogram stats on collection coll since the lastHistogram was
 * recorded is equal to the readDiff, writeDiff, and commandDiff values. Returns the new histogram.
 */
export function assertHistogramDiffEq(db, coll, lastHistogram, readDiff, writeDiff, commandDiff) {
    let thisHistogram = getHistogramStats(coll);
    let diff = diffHistogram(thisHistogram, lastHistogram);
    // Running the $collStats aggregation itself will increment read stats by one.
    assert.eq(diff.reads, readDiff + 1, "miscounted histogram reads");
    assert.eq(diff.writes, writeDiff, "miscounted histogram writes");

    // In some cases, the actual result could contain more results than expected because some
    // background commands could sneak in. For instance, a checkDB command run against a replica set
    // runs an extra "listIndex" command.
    let allowedDiff = 0;
    if (FixtureHelpers.isReplSet(db)) {
        // The checkDB command could be run multiple times in a short period of time.
        allowedDiff = 3;
    }
    assert.lte(Math.abs(diff.commands - commandDiff),
               allowedDiff,
               "miscounted histogram commands:\n" + tojson(diff));
    return thisHistogram;
}

/**
 * Asserts that top contains stats for this collection and returns the recorded stats.
 */
export function getTop(coll) {
    let collName = coll.getFullName();
    let res = coll.getDB().adminCommand("top");
    if (!res.ok) {
        assert.commandFailedWithCode(res, [ErrorCodes.BSONObjectTooLarge, 13548]);
        return undefined;
    }

    assert.eq(true, res.totals.hasOwnProperty(collName), collName + " not found in top");
    return res.totals[collName];
}

/**
 * Returns the difference of the time and count for a given key of two sets of top stats.
 */
export function diffTop(key, thisTop, lastTop) {
    return {
        time: thisTop[key].time - lastTop[key].time,
        count: thisTop[key].count - lastTop[key].count
    };
}

/**
 * Asserts that the count difference of top stats of the key on collection coll since lastTop was
 * recorded is equal to expectedCountDiff. Returns the new top stats.
 */
export function assertTopDiffEq(db, coll, lastTop, key, expectedCountDiff) {
    let thisTop = getTop(coll);
    let diff = diffTop(key, thisTop, lastTop);
    assert.gte(diff.count, 0, "non-decreasing count");
    // TODO (JBR for SERVER-26812): We don't currently guarantee that the time will not be zero
    // which causes occaisional test failures here. If we change the timers in top to use a
    // monotonic clock in SERVER-26812, then we can uncomment these.
    // assert.gte(diff.time, 0, "non-decreasing time");
    // assert.eq(diff.count !== 0, diff.time > 0, "non-zero time iff non-zero count");

    // In some cases, the actual result could contain more results than expected because some
    // background commands could sneak in. For instance, a checkDB command run against a replica set
    // runs an extra "listIndexes" command.
    let allowedDiff = 0;
    if (FixtureHelpers.isReplSet(db)) {
        // The checkDB command could be run mutiple times in a short period of time.
        allowedDiff = 3;
    }
    assert.lte(diff.count - expectedCountDiff,
               allowedDiff,
               "top reports wrong count for commands\n top results: " + tojson(thisTop));
    return thisTop;
}
