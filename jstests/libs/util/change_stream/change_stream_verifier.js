/**
 * Verifier module for black-box testing of change streams.
 *
 * This module orchestrates verification tests for change streams by:
 * - Running one or more VerifierTestCase strategies for a given configuration
 * - Hiding external dependencies behind a VerifierContext facade
 * - Providing composable, reusable test case implementations
 *
 * Main Concepts:
 * - VerifierContext: Facade for reading events, matchers, and clusterTime helpers
 * - VerifierTestCase: Strategy object implementing a particular test
 * - Verifier: Driver that runs one or more test cases with a given context
 *
 * Test Cases:
 * - SingleReaderVerificationTestCase: Validates a single reader's events; supports
 *   allowSkips for ignoreRemovedShards mode
 * - SequentialPairwiseFetchingTestCase: Compares two fetching strategies (e.g., v1 vs v2,
 *   or continuous vs fetch-one-and-resume)
 * - PrefixReadTestCase: Verifies resume-from-clusterTime and prefix correctness
 *
 * Usage:
 * The verifier expects readers to have already been started (potentially in parallel).
 * It reads events from the Connector storage and validates them against matchers.
 */
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";

/**
 * Format a change event record for debugging output.
 * @param {Object} rec - Event record containing changeEvent
 * @param {number} i - Index in the event list
 * @returns {string} Formatted string like "[0] insert @ 123,1"
 */
function formatEventSummary(rec, i) {
    const e = rec.changeEvent;
    return `[${i}] ${e.operationType} @ ${e.clusterTime.t},${e.clusterTime.i}`;
}

/**
 * Context facade for Verifier test cases.
 *
 * Provides a clean interface for:
 * - Reading change events via Connector (after readers have finished)
 * - Retrieving matchers for event comparison
 * - Reading events from specific cluster times for resume testing
 *
 * This abstraction allows test cases to be independent of the underlying
 * infrastructure, making them easier to test and maintain.
 */
class VerifierContext {
    /**
     * Create a VerifierContext.
     * @param {Object} changeStreamReaderConfigs - Map of instanceName -> reader config
     * @param {Object} matchersPerInstance - Map of instanceName -> ChangeStreamMatcher
     * @param {Array} [shardConnections] - Optional array of direct connections to shard primaries.
     *   Required for PrefixReadTestCase to query oplog cluster times for resume testing.
     *   Mongos doesn't expose the oplog, so direct shard connections are needed.
     */
    constructor(changeStreamReaderConfigs, matchersPerInstance, shardConnections = []) {
        this.changeStreamReaderConfigs = changeStreamReaderConfigs;
        this.matchersPerInstance = matchersPerInstance;
        this.shardConnections = shardConnections;
    }

    /**
     * Wait for a reader to finish, then return its recorded events.
     * @param {Mongo} conn - MongoDB connection
     * @param {string} instanceName - Name of the reader instance
     * @returns {Array} Array of change event records {changeEvent, cursorClosed}
     */
    getChangeEvents(conn, instanceName) {
        Connector.waitForDone(conn, instanceName);
        return Connector.readAllChangeEvents(conn, instanceName);
    }

    /**
     * Read change events starting from a specific cluster time.
     * Creates an inline reader with modified configuration for resume testing.
     * @param {Mongo} conn - MongoDB connection
     * @param {string} instanceName - Base instance name (used to find reader config)
     * @param {Timestamp} clusterTime - Cluster time to resume from
     * @param {number} limit - Maximum number of events to read
     * @returns {Array} Array of change event records
     */
    readChangeEventsFromClusterTime(conn, instanceName, clusterTime, limit) {
        const readerInstanceName = this.launchReaderFromClusterTime(conn, instanceName, clusterTime, limit);
        return this.getChangeEvents(conn, readerInstanceName);
    }

    /**
     * Launch a reader from a specific cluster time without waiting for completion.
     * @returns {string} The instance name of the launched reader (use with getChangeEvents to collect).
     */
    launchReaderFromClusterTime(conn, instanceName, clusterTime, limit) {
        const baseCfg = this.changeStreamReaderConfigs[instanceName];
        assert(baseCfg, `No reader config for instanceName=${instanceName}`);
        assert(limit > 0, `limit must be a positive number, got ${limit}`);

        const cfg = Object.assign({}, baseCfg, {
            instanceName: `${instanceName}.resumeFromClusterTime.${clusterTime.t}_${clusterTime.i}`,
            numberOfEventsToRead: limit,
            readingMode: ChangeStreamReadingMode.kContinuous,
            startAtClusterTime: clusterTime,
        });

        ChangeStreamReader.run(conn, cfg);
        return cfg.instanceName;
    }

    /**
     * Get the matcher for a specific reader instance.
     * NOTE: This returns a reference to the matcher, not a copy. If multiple test
     * cases access the same matcher, the state will need to be reset between uses.
     * @param {string} instanceName - Name of the reader instance
     * @returns {ChangeStreamMatcher} Matcher for the instance
     */
    getChangeStreamMatcher(instanceName) {
        const matcher = this.matchersPerInstance[instanceName];
        assert(matcher, `No matcher for instanceName=${instanceName}`);
        return matcher;
    }

    getCommandTrace(instanceName) {
        const cfg = this.changeStreamReaderConfigs[instanceName];
        return cfg ? cfg.debugCommandTrace || [] : [];
    }

    /**
     * Get unique cluster times from the oplog across all shards.
     * Excludes config/admin/control namespaces and empty-namespace entries
     * to avoid resuming from cluster-internal operations that don't produce
     * user-visible change events.
     * @private
     * @param {Timestamp} startTime - Only include cluster times >= this timestamp
     * @param {Timestamp} endTime - Only include cluster times <= this timestamp
     * @returns {Array<Timestamp>} Array of unique cluster times from oplog, sorted
     */
    _getClusterTimesFromOplog(startTime, endTime) {
        assert(
            this.shardConnections.length > 0,
            "No shard connections provided. Pass shardConnections in verifier config.",
        );

        const clusterTimesMap = new Map();

        for (const shardConn of this.shardConnections) {
            const filter = {
                ts: {$gte: startTime},
                ns: {$not: /^(config|admin|change_stream_test_control)\./, $ne: ""},
            };
            if (endTime) {
                filter.ts.$lte = endTime;
            }

            const oplogCursor = shardConn.getDB("local").oplog.rs.find(filter, {ts: 1}).sort({ts: 1});

            while (oplogCursor.hasNext()) {
                const entry = oplogCursor.next();
                if (entry.ts) {
                    const key = `${entry.ts.t},${entry.ts.i}`;
                    if (!clusterTimesMap.has(key)) {
                        clusterTimesMap.set(key, entry.ts);
                    }
                }
            }
        }

        // Convert to array and sort by timestamp value.
        const clusterTimes = Array.from(clusterTimesMap.values());
        clusterTimes.sort((a, b) => {
            if (a.t !== b.t) {
                return a.t - b.t;
            }
            return a.i - b.i;
        });

        return clusterTimes;
    }

    /**
     * Get cluster times for resume testing by reading from the oplog across all shards.
     * This provides comprehensive testing by including timestamps for operations that
     * may not produce change events.
     *
     * Requires shardConnections to be provided in the verifier config.
     *
     * @param {Timestamp} startTime - Start time for oplog query (first event's clusterTime)
     * @param {Timestamp} endTime - End time for oplog query (last event's or invalidate's clusterTime)
     * @returns {Array<Timestamp>} Array of unique cluster times, sorted
     */
    getClusterTimesForResumeTesting(startTime, endTime) {
        assert(this.shardConnections.length > 0, "shardConnections required for resume testing");
        assert(startTime, "startTime is required");
        assert(endTime, "endTime is required");
        return this._getClusterTimesFromOplog(startTime, endTime);
    }
}

/**
 * Assert that the matcher consumed all expected events, with detailed diagnostics on failure.
 * Logs the FSM command trace and reports the first mismatch point.
 */
function assertMatcherDone(matcher, events, ctx, readerInstanceName) {
    if (matcher.isDone()) {
        return;
    }

    const mismatch = matcher.getFirstMismatch();
    const commandTrace = ctx.getCommandTrace(readerInstanceName);
    const actualTypes = events.map((rec) => rec.changeEvent.operationType);
    const expectedGroups = matcher.getExpectedOperationTypes();

    const totalExpected = expectedGroups.reduce((s, g) => s + g.length, 0);
    const expectedLines = expectedGroups.map((g, i) => `  stream ${i}(${g.length}): [${g.join(", ")}]`).join("\n");
    const actualInline = `[${actualTypes.join(", ")}]`;

    jsTest.log.info("FSM command trace (on mismatch)", {
        instanceName: readerInstanceName,
        commandsCount: commandTrace.length,
        commands: commandTrace,
    });

    assert(
        false,
        (mismatch
            ? `Event mismatch at index ${mismatch.index}: expected '${mismatch.expected}', got '${mismatch.actual}'`
            : `Matched ${matcher.getMatchedCount()} of ${totalExpected}`) +
            `\nexpected(${totalExpected}):\n${expectedLines}` +
            `\nactual(${actualTypes.length}): ${actualInline}` +
            `\nGrep logs for "FSM command trace (on mismatch)" to see the full command sequence.`,
    );
}

/**
 * Test case that compares two sequential fetching strategies.
 *
 * This test case is reusable for:
 * - v1 vs v2 change stream comparison
 * - Continuous vs fetch-one-and-resume mode comparison
 * - Any pairwise comparison of change stream reading strategies
 *
 * The test reads events from both instances (which should have been run with
 * different strategies) and verifies they produce identical event sequences.
 */
class SequentialPairwiseFetchingTestCase {
    /**
     * Create a pairwise fetching test case.
     * @param {string} controlInstanceName - Instance name for control reader
     * @param {string} experimentInstanceName - Instance name for experiment reader
     */
    constructor(controlInstanceName, experimentInstanceName) {
        this._controlInstanceName = controlInstanceName;
        this._experimentInstanceName = experimentInstanceName;
    }

    /**
     * Run the pairwise comparison test.
     * @param {Mongo} conn - MongoDB connection
     * @param {VerifierContext} ctx - Verifier context
     */
    run(conn, ctx) {
        const controlEvents = ctx.getChangeEvents(conn, this._controlInstanceName);
        const experimentEvents = ctx.getChangeEvents(conn, this._experimentInstanceName);

        // Verify both readers' events match the expectations defined by the mutation generator.
        for (const [instanceName, events] of [
            [this._controlInstanceName, controlEvents],
            [this._experimentInstanceName, experimentEvents],
        ]) {
            const matcher = ctx.getChangeStreamMatcher(instanceName);
            events.every((rec) => matcher.matches(rec.changeEvent, rec.cursorClosed));
            assertMatcherDone(matcher, events, ctx, instanceName);
        }
    }
}

/**
 * Test case that verifies a single reader's events against expected patterns.
 *
 * In strict mode (default), all expected events must be matched in order.
 * With allowSkips, uses matchesOrSkip() to tolerate missing events (e.g.
 * from a removed shard with ignoreRemovedShards). In that mode only the
 * relative ordering and presence of at least some events are checked.
 */
class SingleReaderVerificationTestCase {
    /**
     * @param {string} readerInstanceName - Instance name for the reader
     * @param {Object} [opts]
     * @param {boolean} [opts.allowSkips=false] - Use matchesOrSkip() instead of matches()
     */
    constructor(readerInstanceName, {allowSkips = false} = {}) {
        this._readerInstanceName = readerInstanceName;
        this._allowSkips = allowSkips;
    }

    run(conn, ctx) {
        const events = ctx.getChangeEvents(conn, this._readerInstanceName);
        const matcher = ctx.getChangeStreamMatcher(this._readerInstanceName);

        if (this._allowSkips) {
            // No assertMatcherDone: with IRS, the removed shard may have held
            // some expected events, so not all matchers will be exhausted.
            events.forEach((rec, i) => {
                assert(
                    matcher.matchesOrSkip(rec.changeEvent, rec.cursorClosed),
                    `${this._readerInstanceName}[${i}]: unexpected '${rec.changeEvent.operationType}' ` +
                        `(ns: ${tojson(rec.changeEvent.ns)}) not in remaining expected events`,
                );
            });
        } else {
            // every() short-circuits on the first mismatch (matches() returns false), so
            // the matcher stops advancing and records the failure point.
            events.every((rec) => matcher.matches(rec.changeEvent, rec.cursorClosed));
            assertMatcherDone(matcher, events, ctx, this._readerInstanceName);
        }
    }
}

/**
 * Test case that verifies prefix/resume correctness.
 *
 * Per spec: "Starting of a change stream from any cluster time test case"
 * 1. Compute cluster times from oplog entries (excluding config/admin/control namespaces)
 * 2. Fetch change events continuously with the default reader
 * 3. Start a change stream from each cluster time and verify that `limit`
 *    fetched events match corresponding events from step 2
 * 4. Verify the full event sequence matches expectations
 *
 * Uses a sliding window of kParallelReadersCount readers to keep throughput
 * high without overwhelming mongos with connections.
 */
class PrefixReadTestCase {
    static kParallelReadersCount = 8;

    constructor(readerInstanceName, limit, {allowSkips = false} = {}) {
        assert(limit > 0, `limit must be a positive number, got ${limit}`);
        this._readerInstanceName = readerInstanceName;
        this._limit = limit;
        this._allowSkips = allowSkips;
    }

    static _extractComparable(rec) {
        return {changeEvent: rec.changeEvent, cursorClosed: rec.cursorClosed};
    }

    _buildWorkItems(events, clusterTimes) {
        const items = [];
        for (const ts of clusterTimes) {
            const startIdx = events.findIndex((rec) => bsonWoCompare(rec.changeEvent.clusterTime, ts) >= 0);
            assert(startIdx >= 0, `No event found with clusterTime >= ${tojson(ts)}, but ts is within event range`);

            const expected = events.slice(startIdx, startIdx + this._limit).map(PrefixReadTestCase._extractComparable);
            if (expected.length > 0) {
                items.push({ts, expected});
            }
        }
        return items;
    }

    _launchReader(conn, ctx, item) {
        return {
            ...item,
            readerInstanceName: ctx.launchReaderFromClusterTime(
                conn,
                this._readerInstanceName,
                item.ts,
                item.expected.length,
            ),
        };
    }

    _verifyReader(conn, ctx, item) {
        const actual = ctx.getChangeEvents(conn, item.readerInstanceName).map(PrefixReadTestCase._extractComparable);
        const expected = item.expected.slice(0, actual.length);
        assert(
            bsonUnorderedFieldsCompare(expected, actual) === 0,
            `mismatch when resuming from clusterTime ${tojson(item.ts)}`,
            {clusterTime: item.ts, expected, actual},
        );
    }

    run(conn, ctx) {
        const events = ctx.getChangeEvents(conn, this._readerInstanceName);
        if (events.length === 0) {
            // IRS (ignoreRemovedShards) drain readers can legitimately see zero
            // events when the removed shard held all data for this collection.
            assert(this._allowSkips, "No events captured but allowSkips is false — this indicates a real bug");
            return;
        }

        const startTime = events[0].changeEvent.clusterTime;
        const endTime = events[events.length - 1].changeEvent.clusterTime;

        const clusterTimes = ctx.getClusterTimesForResumeTesting(startTime, endTime);
        const workItems = this._buildWorkItems(events, clusterTimes);

        jsTest.log.info("PrefixReadTestCase: starting resume verification", {
            clusterTimes: clusterTimes.length,
            workItems: workItems.length,
            parallelReaders: PrefixReadTestCase.kParallelReadersCount,
        });

        // Sliding window: always keep kParallelReadersCount readers in flight.
        // As each reader completes verification, the next one is launched.
        const inflight = [];
        let next = 0;
        while (next < workItems.length && inflight.length < PrefixReadTestCase.kParallelReadersCount) {
            inflight.push(this._launchReader(conn, ctx, workItems[next++]));
        }
        while (inflight.length > 0) {
            this._verifyReader(conn, ctx, inflight.shift());
            if (next < workItems.length) {
                inflight.push(this._launchReader(conn, ctx, workItems[next++]));
            }
        }

        const matcher = ctx.getChangeStreamMatcher(this._readerInstanceName);
        if (this._allowSkips) {
            // No assertMatcherDone: with IRS, the removed shard may have held
            // some expected events, so not all matchers will be exhausted.
            events.forEach((rec, i) => {
                assert(
                    matcher.matchesOrSkip(rec.changeEvent, rec.cursorClosed),
                    `${this._readerInstanceName}[${i}]: unexpected '${rec.changeEvent.operationType}' ` +
                        `(ns: ${tojson(rec.changeEvent.ns)}) not in remaining expected events`,
                );
            });
        } else {
            events.every((rec) => matcher.matches(rec.changeEvent, rec.cursorClosed));
            assertMatcherDone(matcher, events, ctx, this._readerInstanceName);
        }
    }
}

/**
 * Verifier driver class.
 *
 * Orchestrates running one or more test cases with a given configuration.
 * Expects readers to have already been started (potentially in parallel).
 */
class Verifier {
    /**
     * Run verification tests with the given configuration and test cases.
     * @param {Mongo} conn - MongoDB connection
     * @param {Object} config - Verifier configuration containing:
     *   - changeStreamReaderConfigs: Map of instanceName -> reader config
     *   - matcherSpecsByInstance: Map of instanceName -> matcher
     *   - instanceName: Name for this verifier instance (for notifyDone)
     *   - shardConnections: (optional) Direct connections to shard primaries for oplog access.
     *       Required by PrefixReadTestCase to query cluster times for resume testing.
     * @param {Array} testCases - Array of test cases to run
     */
    run(conn, config, testCases) {
        const ctx = new VerifierContext(
            config.changeStreamReaderConfigs,
            config.matcherSpecsByInstance,
            config.shardConnections || [],
        );

        for (const testCase of testCases) {
            testCase.run(conn, ctx);
        }

        Connector.notifyDone(conn, config.instanceName);
    }
}

export {
    VerifierContext,
    SequentialPairwiseFetchingTestCase,
    SingleReaderVerificationTestCase,
    PrefixReadTestCase,
    Verifier,
};
