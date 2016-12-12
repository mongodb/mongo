// Utility functions for obtaining and diffing top and latency histogram statistics.

/**
 * Returns the latency histograms for the given collection.
 */
function getHistogramStats(coll) {
    return coll.latencyStats().next().latencyStats;
}

/**
 * Returns the differences in read, write, and command counts between two histograms.
 */
function diffHistogram(coll, thisHistogram, lastHistogram) {
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
function assertHistogramDiffEq(coll, lastHistogram, readDiff, writeDiff, commandDiff) {
    let thisHistogram = getHistogramStats(coll);
    let diff = diffHistogram(coll, thisHistogram, lastHistogram);
    // Running the $collStats aggregation itself will increment read stats by one.
    assert.eq(diff.reads, readDiff + 1, "miscounted histogram reads");
    assert.eq(diff.writes, writeDiff, "miscounted histogram writes");
    assert.eq(diff.commands, commandDiff, "miscounted histogram commands");
    return thisHistogram;
}

/**
 * Asserts that top contains stats for this collection and returns the recorded stats.
 */
function getTop(coll) {
    let collName = coll.getFullName();
    let res = coll.getDB().adminCommand("top");
    assert.commandWorked(res);
    assert.eq(true, res.totals.hasOwnProperty(collName), collName + " not found in top");
    return res.totals[collName];
}

/**
 * Returns the difference of the time and count for a given key of two sets of top stats.
 */
function diffTop(key, thisTop, lastTop) {
    return {
        time: thisTop[key].time - lastTop[key].time,
        count: thisTop[key].count - lastTop[key].count
    };
}

/**
 * Asserts that the count difference of top stats of the key on collection coll since lastTop was
 * recorded is equal to expectedCountDiff. Returns the new top stats.
 */
function assertTopDiffEq(coll, lastTop, key, expectedCountDiff) {
    let thisTop = getTop(coll);
    let diff = diffTop(key, thisTop, lastTop);
    assert.gte(diff.count, 0, "non-decreasing count");
    // TODO (JBR for SERVER-26812): We don't currently guarantee that the time will not be zero
    // which causes occaisional test failures here. If we change the timers in top to use a
    // monotonic clock in SERVER-26812, then we can uncomment these.
    // assert.gte(diff.time, 0, "non-decreasing time");
    // assert.eq(diff.count !== 0, diff.time > 0, "non-zero time iff non-zero count");
    assert.eq(diff.count, expectedCountDiff, "top reports wrong count for " + key);
    return thisTop;
}
