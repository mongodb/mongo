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
     * Get change events for a specific reader instance.
     * Waits for the reader to complete before fetching events.
     * @param {Mongo} conn - MongoDB connection
     * @param {string} instanceName - Name of the reader instance
     * @returns {Array} Array of change event records {changeEvent, cursorClosed}
     */
    getChangeEvents(conn, instanceName) {
        // Ensure reader is done reading events before fetching them.
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
        const baseCfg = this.changeStreamReaderConfigs[instanceName];
        assert(baseCfg, `No reader config for instanceName=${instanceName}`);
        assert(limit > 0, `limit must be a positive number, got ${limit}`);

        const cfg = Object.assign({}, baseCfg, {
            // Do NOT reuse instanceName here, this is an inline verifier read.
            instanceName: `${instanceName}.resumeFromClusterTime.${clusterTime.t}_${clusterTime.i}`,
            numberOfEventsToRead: limit,
            readingMode: ChangeStreamReadingMode.kContinuous,
            startAtClusterTime: clusterTime,
        });

        ChangeStreamReader.run(conn, cfg);
        return this.getChangeEvents(conn, cfg.instanceName);
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

    /**
     * Get all unique cluster times from the oplog across all shards.
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

        // Use Map with string keys instead of Set because JavaScript Set uses reference equality
        // for objects. Two Timestamp objects with identical {t, i} values would be considered
        // distinct in a Set since they're different object instances. The string key format
        // "t,i" provides value-based deduplication.
        const clusterTimesMap = new Map();

        for (const shardConn of this.shardConnections) {
            // Build query filter for oplog entries.
            const filter = {ts: {$gte: startTime}};
            if (endTime) {
                filter.ts.$lte = endTime;
            }

            // Read oplog entries from this shard.
            const oplogCursor = shardConn.getDB("local").oplog.rs.find(filter).sort({ts: 1});

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

        // Build summary lists for debugging (operationType @ clusterTime).
        const controlSummary = controlEvents.map(formatEventSummary);
        const experimentSummary = experimentEvents.map(formatEventSummary);

        // Both readers should read the same number of events.
        assert.eq(
            experimentEvents.length,
            controlEvents.length,
            `${this._controlInstanceName} (${controlEvents.length} events) and ` +
                `${this._experimentInstanceName} (${experimentEvents.length} events) should have same event count.\n` +
                `Control events: ${tojson(controlSummary)}\n` +
                `Experiment events: ${tojson(experimentSummary)}`,
        );

        // Extract just the changeEvent and cursorClosed from each record for comparison.
        // Exclude storage _id since each reader stores events with different document IDs.
        const extractComparable = (rec) => ({changeEvent: rec.changeEvent, cursorClosed: rec.cursorClosed});
        const experimentComparable = experimentEvents.map(extractComparable);
        const controlComparable = controlEvents.map(extractComparable);

        // Verify experiment events exactly match control events.
        let firstMismatchIdx = -1;
        for (let i = 0; i < experimentComparable.length; i++) {
            if (bsonUnorderedFieldsCompare(experimentComparable[i], controlComparable[i]) !== 0) {
                firstMismatchIdx = i;
                break;
            }
        }

        assert(
            firstMismatchIdx === -1,
            `${this._controlInstanceName} and ${this._experimentInstanceName} outputs differ at event ${firstMismatchIdx}`,
        );

        // Verify that change event sequence produced by '_experimentInstanceName' matches
        // the expectations defined by mutation generator.
        const matcher = ctx.getChangeStreamMatcher(this._experimentInstanceName);
        for (let i = 0; i < experimentEvents.length; i++) {
            const rec = experimentEvents[i];
            matcher.matches(rec.changeEvent, rec.cursorClosed);
        }

        // Accept partial match if the experiment reader stopped before trailing
        // invalidate/rename/drop events (which cannot be resumed for collection-level streams).
        const lastEvent = experimentEvents[experimentEvents.length - 1];
        const endsWithInvalidate =
            lastEvent && lastEvent.changeEvent && lastEvent.changeEvent.operationType === "invalidate";
        if (!endsWithInvalidate) {
            // TODO SERVER-117490: Use assertDone() to support deferred matching for out-of-order
            // per-shard events.
            matcher.assertDone();
        }
    }
}

/**
 * Test case that compares two readers but filters out duplicate events before comparison.
 *
 * WHY THIS IS NEEDED:
 * In sharded clusters, certain DDL operations (like dropIndexes) can emit multiple change
 * stream events with the same clusterTime and operationType - one per shard that participates
 * in the operation. When using the Continuous reading mode, all these duplicate events are
 * captured because the cursor stays open.
 *
 * However, when using FetchOneAndResume mode (open cursor, read one event, close, resume),
 * the `resumeAfter` token-based resumption cannot reliably distinguish between events that
 * share the same clusterTime. This causes FetchOneAndResume to skip some of these duplicates.
 *
 * To compare Continuous vs FetchOneAndResume fairly, this test case filters out duplicate
 * events (same clusterTime.t + clusterTime.i + operationType) from the Control (Continuous)
 * stream before comparing with the Experiment (FetchOneAndResume) stream.
 *
 * EXAMPLE:
 * - Continuous sees: [dropIndexes@t1, dropIndexes@t1, insert@t2] (3 events)
 * - FetchOneAndResume sees: [dropIndexes@t1, insert@t2] (2 events, second dropIndexes skipped)
 * - After filtering Control: [dropIndexes@t1, insert@t2] → now both match
 */
class DuplicateFilteringPairwiseTestCase {
    /**
     * Create a duplicate-filtering pairwise test case.
     * @param {string} controlInstanceName - Instance name for control reader (Continuous)
     * @param {string} experimentInstanceName - Instance name for experiment reader (FetchOneAndResume)
     */
    constructor(controlInstanceName, experimentInstanceName) {
        this._controlInstanceName = controlInstanceName;
        this._experimentInstanceName = experimentInstanceName;
    }

    /**
     * Run the duplicate-filtering comparison test.
     * @param {Mongo} conn - MongoDB connection
     * @param {VerifierContext} ctx - Verifier context
     */
    run(conn, ctx) {
        const controlEvents = ctx.getChangeEvents(conn, this._controlInstanceName);
        const experimentEvents = ctx.getChangeEvents(conn, this._experimentInstanceName);

        // Filter Control events to remove per-shard duplicate DDL events.
        // In sharded clusters, DDL operations like createIndexes/dropIndexes emit one event
        // per shard. We deduplicate only these DDL event types to get a single logical event.
        // DML events (insert/update/delete) are NOT deduplicated since concurrent operations
        // on different shards at the same clusterTime are legitimate distinct events.
        const ddlEventTypes = new Set([
            "createIndexes",
            "dropIndexes",
            "shardCollection",
            "reshardCollection",
            "create",
            "drop",
            "rename",
            "dropDatabase",
            "modify",
            "refineCollectionShardKey",
        ]);
        const seenKeys = new Set();
        const filteredControlEvents = [];
        for (const rec of controlEvents) {
            const e = rec.changeEvent;
            // Only deduplicate DDL events; pass through all DML events.
            if (ddlEventTypes.has(e.operationType)) {
                const key = `${e.clusterTime.t},${e.clusterTime.i},${e.operationType}`;
                if (!seenKeys.has(key)) {
                    seenKeys.add(key);
                    filteredControlEvents.push(rec);
                }
            } else {
                filteredControlEvents.push(rec);
            }
        }

        // Build summary lists for debugging.
        const controlSummary = filteredControlEvents.map(formatEventSummary);
        const experimentSummary = experimentEvents.map(formatEventSummary);

        // Verify counts match.
        assert.eq(
            filteredControlEvents.length,
            experimentEvents.length,
            `Event count mismatch after filtering duplicates: Control=${filteredControlEvents.length}, Experiment=${experimentEvents.length}`,
        );

        // Compare each event (operationType and clusterTime).
        for (let i = 0; i < experimentEvents.length; i++) {
            const control = filteredControlEvents[i].changeEvent;
            const experiment = experimentEvents[i].changeEvent;

            assert(
                control.operationType === experiment.operationType &&
                    control.clusterTime.t === experiment.clusterTime.t &&
                    control.clusterTime.i === experiment.clusterTime.i,
                `Event ${i} mismatch: Control=${control.operationType}@${control.clusterTime.t},${control.clusterTime.i}, ` +
                    `Experiment=${experiment.operationType}@${experiment.clusterTime.t},${experiment.clusterTime.i}`,
            );
        }
    }
}

/**
 * Test case that verifies a single reader's events against expected patterns.
 *
 * This is the simplest test case - it just reads events from one instance
 * and verifies they match the expected patterns using the configured matcher.
 *
 * Used for basic change stream verification without comparing multiple strategies.
 */
class SingleReaderVerificationTestCase {
    /**
     * Create a single reader verification test case.
     * @param {string} readerInstanceName - Instance name for the reader
     */
    constructor(readerInstanceName) {
        this._readerInstanceName = readerInstanceName;
    }

    /**
     * Run the verification test.
     * @param {Mongo} conn - MongoDB connection
     * @param {VerifierContext} ctx - Verifier context
     */
    run(conn, ctx) {
        const events = ctx.getChangeEvents(conn, this._readerInstanceName);

        // Verify that change event sequence matches the expectations.
        const matcher = ctx.getChangeStreamMatcher(this._readerInstanceName);
        for (const rec of events) {
            matcher.matches(rec.changeEvent, rec.cursorClosed);
        }
        // TODO SERVER-117490: Use assertDone() to support deferred matching for out-of-order
        // per-shard events.
        matcher.assertDone();
    }
}

/**
 * Test case that verifies prefix/resume correctness.
 *
 * Per spec: "Starting of a change stream from any cluster time test case"
 * 1. Compute a set of cluster times used by the cluster during mutation execution
 * 2. Fetch change events continuously with the default reader
 * 3. Start a change stream from every used cluster time and verify that 3 fetched
 *    events match corresponding events from step 2
 * 4. Verify the full event sequence matches expectations
 *
 * IMPORTANT: We stop testing at the first invalidate event because:
 * - Collection-level change streams cannot be opened on a dropped collection
 * - startAtOperationTime only works if the collection still exists
 * - After a drop/rename (invalidate), we cannot resume a collection-level stream
 */
class PrefixReadTestCase {
    /**
     * Create a prefix read test case.
     * @param {string} readerInstanceName - Instance name for the reader
     * @param {number} limit - Number of events to read for each prefix test
     */
    constructor(readerInstanceName, limit) {
        assert(limit > 0, `limit must be a positive number, got ${limit}`);
        this._readerInstanceName = readerInstanceName;
        this._limit = limit;
    }

    /**
     * Run the prefix read test.
     * @param {Mongo} conn - MongoDB connection
     * @param {VerifierContext} ctx - Verifier context
     */
    run(conn, ctx) {
        const events = ctx.getChangeEvents(conn, this._readerInstanceName);

        // Find the cluster time of the first invalidate event (if any).
        // We must stop testing BEFORE this point because:
        // - An invalidate signals that the collection was dropped/renamed
        // - After this, we cannot open a new collection-level change stream
        //   using startAtOperationTime (the collection no longer exists)
        let invalidateClusterTime = null;
        for (const rec of events) {
            if (rec.changeEvent && rec.changeEvent.operationType === "invalidate") {
                invalidateClusterTime = rec.changeEvent.clusterTime;
                break;
            }
        }

        // Get cluster times for resume testing from the oplog.
        const startTime = events[0]?.changeEvent?.clusterTime;
        const endTime = invalidateClusterTime || events[events.length - 1]?.changeEvent?.clusterTime;
        const clusterTimes = ctx.getClusterTimesForResumeTesting(startTime, endTime);

        // Filter cluster times to only those BEFORE the invalidate.
        const validClusterTimes = invalidateClusterTime
            ? clusterTimes.filter((ts) => bsonWoCompare(ts, invalidateClusterTime) < 0)
            : clusterTimes;

        // Extract just the changeEvent and cursorClosed from each record for comparison.
        const extractComparable = (rec) => ({changeEvent: rec.changeEvent, cursorClosed: rec.cursorClosed});

        // For each valid cluster time, start a change stream and verify events match.
        for (let i = 0; i < validClusterTimes.length; i++) {
            const ts = validClusterTimes[i];

            // Find events with clusterTime >= ts (what we expect to see).
            // When using oplog-based cluster times, some timestamps may not have corresponding
            // change events (they represent operations that don't produce change events).
            const expectedStartIdx = events.findIndex((rec) => bsonWoCompare(rec.changeEvent.clusterTime, ts) >= 0);
            assert(
                expectedStartIdx >= 0,
                `No event found with clusterTime >= ${tojson(ts)}, but ts is within event range`,
            );

            // Get up to 'limit' events, but stop before invalidate.
            let endIdx = expectedStartIdx + this._limit;
            const invalidateIdx = events.findIndex((rec) => rec.changeEvent.operationType === "invalidate");
            if (invalidateIdx >= 0 && endIdx > invalidateIdx) {
                endIdx = invalidateIdx; // Don't include invalidate or beyond.
            }

            const expected = events.slice(expectedStartIdx, endIdx).map(extractComparable);
            if (expected.length === 0) {
                continue;
            }

            const actual = ctx
                .readChangeEventsFromClusterTime(conn, this._readerInstanceName, ts, expected.length)
                .map(extractComparable);

            // Use bsonUnorderedFieldsCompare to ignore field order differences.
            assert(
                bsonUnorderedFieldsCompare(expected, actual) === 0,
                `mismatch when resuming from clusterTime ${tojson(ts)}`,
                {clusterTime: ts, expected, actual},
            );
        }

        // Verify that change event sequence produced by '_readerInstanceName' matches
        // the expectations defined by mutation generator.
        const matcher = ctx.getChangeStreamMatcher(this._readerInstanceName);
        for (const rec of events) {
            matcher.matches(rec.changeEvent, rec.cursorClosed);
        }
        // TODO SERVER-117490: Use assertDone() to support deferred matching for out-of-order
        // per-shard events.
        matcher.assertDone();
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
    DuplicateFilteringPairwiseTestCase,
    SingleReaderVerificationTestCase,
    PrefixReadTestCase,
    Verifier,
};
