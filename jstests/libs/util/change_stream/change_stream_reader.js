/**
 * ChangeStreamReader module for reading change stream events in tests.
 *
 * Responsibilities:
 * - Opens a change stream (collection, database, or cluster scope) according to config.
 * - Reads events in either v1 or v2 format.
 * - Reads events either continuously or in fetch-one-and-resume mode.
 * - Writes each event via the Connector interface into a dedicated collection.
 * - Signals completion via Connector.notifyDone(instanceName).
 */
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {ChangeStreamTest, ChangeStreamWatchMode, isInvalidated} from "jstests/libs/query/change_stream_util.js";
import {Thread} from "jstests/libs/parallelTester.js";

/**
 * Best-effort cursor cleanup. The server kills cursors after invalidate, so
 * killCursors may fail with "cursor not found" — that's expected and benign.
 */
function tryCleanUp(cst, instanceName) {
    if (!cst) {
        return;
    }
    try {
        cst.cleanUp();
    } catch (e) {
        jsTest.log.info("ChangeStreamReader cleanUp failed (benign)", {
            instanceName,
            error: e.message,
        });
    }
}

/**
 * Reading mode constants.
 */
const ChangeStreamReadingMode = {
    kContinuous: "continuous",
    kFetchOneAndResume: "fetchOneAndResume",
};

/**
 * ChangeStreamReader class for reading and recording change stream events.
 */
class ChangeStreamReader {
    /** Default batch size for cursor getMore operations. */
    static kDefaultGetMoreBatchSize = 1;

    /** All threads spawned by run(), must be joined before shell exit. */
    static _threads = [];

    /**
     * Spawn the reader in a background Thread.
     *
     * The config object is serialized to BSON and passed to the thread, which
     * creates its own Mongo connection and runs the reader logic. Callers
     * synchronize via Connector.waitForDone(conn, config.instanceName).
     *
     * Threads are tracked internally -- call joinAll() before test teardown.
     *
     * @param {Mongo} conn - MongoDB connection (host is extracted for the thread).
     * @param {Object} config - BSON-serializable configuration (see _execute for fields).
     */
    static run(conn, config) {
        const host = conn.host;
        const thread = new Thread(
            async function (host, config) {
                const {ChangeStreamReader} = await import("jstests/libs/util/change_stream/change_stream_reader.js");
                const {Connector} = await import("jstests/libs/util/change_stream/change_stream_connector.js");
                const conn = new Mongo(host);
                try {
                    ChangeStreamReader._execute(conn, config);
                } catch (e) {
                    jsTest.log.info("ChangeStreamReader thread FAILED", {
                        instanceName: config.instanceName,
                        error: e.toString(),
                        stack: e.stack,
                    });
                    Connector.notifyDone(conn, config.instanceName);
                    throw e;
                }
            },
            host,
            config,
        );
        thread.start();
        ChangeStreamReader._threads.push(thread);
    }

    /**
     * Join all threads spawned by run().
     * Must be called before the test exits to avoid shell abort (SIGABRT).
     * Safe to call multiple times -- clears the list after joining.
     */
    static joinAll() {
        const threads = ChangeStreamReader._threads;
        ChangeStreamReader._threads = [];
        const errors = [];
        for (const t of threads) {
            try {
                t.join();
            } catch (e) {
                errors.push(e);
            }
        }
        if (errors.length > 0) {
            jsTest.log.error("ChangeStreamReader threads failed", {errors});
            throw new Error(
                `${errors.length} ChangeStreamReader thread(s) failed: ${errors.map((e) => e.toString()).join("; ")}`,
            );
        }
    }

    /**
     * Internal entry point called by run() via the background Thread.
     *
     * @param {Mongo} conn - MongoDB connection.
     * @param {Object} config - Configuration object containing:
     *   - instanceName: Name of the reader instance (used as collection name for storing events).
     *   - watchMode: ChangeStreamWatchMode value (kCollection, kDb, or kCluster).
     *   - dbName: Database name (required for Collection and Database types).
     *   - collName: Collection name (required for Collection type).
     *   - numberOfEventsToRead: Number of events to read before stopping.
     *   - readingMode: ChangeStreamReadingMode value. Default: Continuous.
     *   - showExpandedEvents: Optional boolean to show expanded events (default: true).
     *   - showSystemEvents: Optional boolean to show system events (default: false).
     *   - batchSize: Optional cursor batch size for getMore operations (default: 1).
     *   - excludeOperationTypes: Optional array of operation types to filter out at the
     *       pipeline level (e.g., ["createIndexes", "dropIndexes"]). Use this for tests
     *       that don't want to deal with unpredictable per-shard event counts.
     *   - startAtClusterTime: Optional Timestamp to start at (reconstructed after
     *       thread boundary crossing).
     * @private
     */
    static _execute(conn, config) {
        // Timestamp loses its BSON type when crossing thread boundaries.
        if (config.startAtClusterTime && !(config.startAtClusterTime instanceof Timestamp)) {
            const ts = config.startAtClusterTime;
            config.startAtClusterTime = new Timestamp(ts.t, ts.i);
        }

        switch (config.readingMode) {
            case ChangeStreamReadingMode.kContinuous:
                ChangeStreamReader._readContinuous(conn, config);
                break;
            case ChangeStreamReadingMode.kFetchOneAndResume:
                ChangeStreamReader._readFetchOneAndResume(conn, config);
                break;
            default:
                throw new Error(`Unknown change stream reading mode: ${config.readingMode}`);
        }

        Connector.notifyDone(conn, config.instanceName);
    }

    /**
     * Open a change stream using ChangeStreamTest utility.
     * @param {Mongo} conn - MongoDB connection.
     * @param {Object} config - Configuration object.
     * @param {Object|null} resumeToken - Resume token to continue from.
     * @param {boolean} useStartAfter - Use startAfter instead of resumeAfter (required after invalidate).
     * @private
     */
    static _openChangeStream(conn, config, resumeToken = null, useStartAfter = false) {
        const db =
            config.watchMode === ChangeStreamWatchMode.kCluster ? conn.getDB("admin") : conn.getDB(config.dbName);

        const cst = new ChangeStreamTest(db);

        const changeStreamSpec = {
            showExpandedEvents: config.showExpandedEvents ?? true,
            ...(config.showSystemEvents ? {showSystemEvents: true} : {}),
        };
        if (config.version) {
            changeStreamSpec.version = config.version;
        }
        if (config.watchMode === ChangeStreamWatchMode.kCluster) {
            changeStreamSpec.allChangesForCluster = true;
        }
        // Use startAfter for invalidate tokens, resumeAfter for normal tokens.
        if (resumeToken) {
            if (useStartAfter) {
                changeStreamSpec.startAfter = resumeToken;
            } else {
                changeStreamSpec.resumeAfter = resumeToken;
            }
        } else if (config.startAtClusterTime) {
            changeStreamSpec.startAtOperationTime = config.startAtClusterTime;
        }

        const pipeline = [{$changeStream: changeStreamSpec}];

        // For cluster-wide change streams, filter out events from control database.
        // Database-level streams don't need this since they only watch the test database.
        if (config.watchMode === ChangeStreamWatchMode.kCluster) {
            pipeline.push({$match: {"ns.db": {$ne: Connector.controlDatabase}}});
        }

        // Filter out specified operation types at the pipeline level if requested.
        // This avoids unpredictable per-shard event counts in multi-shard clusters.
        if (config.excludeOperationTypes && config.excludeOperationTypes.length > 0) {
            pipeline.push({$match: {operationType: {$nin: config.excludeOperationTypes}}});
        }

        const cursorOptions = {};
        if (config.batchSize !== undefined) {
            cursorOptions.batchSize = config.batchSize;
        }

        const watchOptions = {
            pipeline: pipeline,
            collection: config.watchMode === ChangeStreamWatchMode.kCollection ? config.collName : 1,
            aggregateOptions: {cursor: cursorOptions},
        };

        const cursor = cst.startWatchingChanges(watchOptions);
        return {cst, cursor};
    }

    static kFCVRetryableErrors = [
        ErrorCodes.QueryPlanKilled,
        ErrorCodes.ConflictingOperationInProgress,
        ErrorCodes.Interrupted,
    ];

    /**
     * Open (or reopen) a change stream and read one event. On FCV-related
     * transient errors (only when bgMutator is enabled), cleans up and retries.
     * Non-retryable errors are thrown immediately. This is the single place
     * where transient error retry lives.
     *
     * @param {Mongo} conn - MongoDB connection.
     * @param {Object} cfg - Reader configuration.
     * @param {Object|null} cst - Existing ChangeStreamTest (null to force open).
     * @param {Object|null} cursor - Existing cursor (null to force open).
     * @param {Object|null} resumeToken - Token to resume from when reopening.
     * @param {boolean} wasInvalidate - Use startAfter instead of resumeAfter.
     * @returns {{ changeEvent, cst, cursor }}
     */
    static _readOneEvent(conn, cfg, cst, cursor, resumeToken, wasInvalidate) {
        let result;
        assert.soon(() => {
            try {
                if (!cst || !cursor) {
                    ({cst, cursor} = ChangeStreamReader._openChangeStream(conn, cfg, resumeToken, wasInvalidate));
                }
                // Always use skipFirst=false to check the current batch before issuing getMore.
                // This ensures we don't miss events in firstBatch (after open) or nextBatch.
                const changeEvent = cst.getNextChanges(cursor, 1, false)[0];
                result = {changeEvent, cst, cursor};
                return true;
            } catch (e) {
                if (!TestData.enableBgMutator || !ChangeStreamReader.kFCVRetryableErrors.includes(e.code)) {
                    throw e;
                }
                jsTest.log.info("ChangeStreamReader FCV error, will retry", {
                    instanceName: cfg.instanceName,
                    code: e.code,
                    error: e.message,
                });
                tryCleanUp(cst, cfg.instanceName);
                cst = null;
                cursor = null;
                return false;
            }
        }, `ChangeStreamReader [${cfg.instanceName}]: timed out reading event`);
        return result;
    }

    /**
     * Validate and record a change event. Shared by both reading modes.
     */
    static _processEvent(conn, cfg, changeEvent, count, readEventTypes) {
        assert(changeEvent, `Expected change event at index ${count}, but got none`);
        assert(changeEvent._id, `Change event at index ${count} missing _id (resume token)`);

        const isInvalidate = isInvalidated(changeEvent);
        readEventTypes.push(changeEvent.operationType);

        jsTest.log.info("ChangeStreamReader Read event", {
            instanceName: cfg.instanceName,
            eventIndex: count + 1,
            total: cfg.numberOfEventsToRead,
            operationType: changeEvent.operationType,
        });

        // cursorClosed is true for invalidate events (server closes cursor after invalidate).
        Connector.writeChangeEvent(conn, cfg.instanceName, {
            changeEvent,
            cursorClosed: isInvalidate,
        });

        return isInvalidate;
    }

    /**
     * Read events continuously, keeping the cursor open.
     * Handles invalidate events by reopening the cursor with startAfter.
     * @private
     */
    static _readContinuous(conn, cfg) {
        jsTest.log.info("ChangeStreamReader Starting continuous read", cfg);
        let cst = null;
        let cursor = null;
        const readEventTypes = [];
        let lastResumeToken = null;
        let lastWasInvalidate = false;

        for (let count = 0; count < cfg.numberOfEventsToRead; count++) {
            const result = ChangeStreamReader._readOneEvent(conn, cfg, cst, cursor, lastResumeToken, lastWasInvalidate);
            cst = result.cst;
            cursor = result.cursor;

            const isInvalidate = ChangeStreamReader._processEvent(conn, cfg, result.changeEvent, count, readEventTypes);
            lastResumeToken = result.changeEvent._id;
            lastWasInvalidate = isInvalidate;

            if (isInvalidate) {
                tryCleanUp(cst, cfg.instanceName);
                cst = null;
                cursor = null;
            }
        }

        jsTest.log.info("ChangeStreamReader Read events", {instanceName: cfg.instanceName, readEventTypes});
        tryCleanUp(cst, cfg.instanceName);
    }

    /**
     * Read events one at a time, closing and reopening the cursor after each event.
     * Uses resumeAfter with the previous event's token.
     * After an invalidate, uses startAfter to reopen the cursor.
     * @private
     */
    static _readFetchOneAndResume(conn, cfg) {
        jsTest.log.info("ChangeStreamReader Starting fetch-one-and-resume", cfg);
        let resumeToken = null;
        let useStartAfter = false;
        const readEventTypes = [];

        for (let count = 0; count < cfg.numberOfEventsToRead; count++) {
            const result = ChangeStreamReader._readOneEvent(conn, cfg, null, null, resumeToken, useStartAfter);

            const isInvalidate = ChangeStreamReader._processEvent(conn, cfg, result.changeEvent, count, readEventTypes);
            resumeToken = result.changeEvent._id;
            useStartAfter = isInvalidate;

            tryCleanUp(result.cst, cfg.instanceName);
        }

        jsTest.log.info("ChangeStreamReader Read events", {instanceName: cfg.instanceName, readEventTypes});
    }
}

export {ChangeStreamReader, ChangeStreamReadingMode};
