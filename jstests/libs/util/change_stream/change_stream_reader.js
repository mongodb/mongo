/**
 * ChangeStreamReader module for reading change stream events in tests.
 *
 * Responsibilities:
 * - Opens a change stream (collection, database, or cluster scope) according to config.
 * - Reads events in either v1 or v2 format.
 * - Supports four reading modes: continuous, fetch-one-and-resume, and drain
 *   variants of each (read until no more events are available).
 * - Writes each event via the Connector interface into a dedicated collection.
 * - Signals completion via Connector.notifyDone(instanceName).
 */
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {
    ChangeStreamTest,
    ChangeStreamWatchMode,
    isInvalidated,
} from "jstests/libs/query/change_stream_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {DedupCommandEventTracker} from "jstests/libs/util/change_stream/change_stream_reader_dedup.js";

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
        jsTest.log.debug("ChangeStreamReader cleanUp failed (benign)", {
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
    kFetchOneAndResumeFromPbrt: "fetchOneAndResumeFromPbrt",
    kReadUntilDone: "readUntilDone",
    kFetchOneAndResumeUntilDone: "fetchOneAndResumeUntilDone",
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
                const {ChangeStreamReader} = await import(
                    "jstests/libs/util/change_stream/change_stream_reader.js"
                );
                const {Connector} = await import(
                    "jstests/libs/util/change_stream/change_stream_connector.js"
                );
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
     *   - dedupSeedKeys: Optional dedup keys seen before `startAtClusterTime` for a
     *       mid-stream reader.
     * @private
     */
    static _execute(conn, config) {
        // Timestamp loses its BSON type when crossing thread boundaries.
        if (config.startAtClusterTime && !(config.startAtClusterTime instanceof Timestamp)) {
            const ts = config.startAtClusterTime;
            config.startAtClusterTime = new Timestamp(ts.t, ts.i);
        }
        // Reader state shared by all read paths. The dedup tracker is created once and is
        // always on: a duplicate command (DDL) event is a server-retry artifact, never a
        // legitimate expected event. All raw events enter the reader through _nextEvent().
        const st = {
            conn,
            cfg: config,
            dedup: new DedupCommandEventTracker(config.dedupSeedKeys),
            cst: null,
            cursor: null,
            resumeToken: null,
            useStartAfter: false,
        };
        switch (config.readingMode) {
            case ChangeStreamReadingMode.kContinuous:
                ChangeStreamReader._readContinuous(st);
                break;
            case ChangeStreamReadingMode.kFetchOneAndResume:
                ChangeStreamReader._readFetchOneAndResume(st);
                break;
            case ChangeStreamReadingMode.kFetchOneAndResumeFromPbrt:
                ChangeStreamReader._readFetchOneAndResume(st, /*resumeFromPbrt=*/ true);
                break;
            case ChangeStreamReadingMode.kReadUntilDone:
                ChangeStreamReader._drainEvents(st, false);
                break;
            case ChangeStreamReadingMode.kFetchOneAndResumeUntilDone:
                ChangeStreamReader._drainEvents(st, true);
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
            config.watchMode === ChangeStreamWatchMode.kCluster
                ? conn.getDB("admin")
                : conn.getDB(config.dbName);

        const cst = new ChangeStreamTest(db);

        const changeStreamSpec = {
            showExpandedEvents: config.showExpandedEvents ?? true,
            ...(config.showSystemEvents ? {showSystemEvents: true} : {}),
        };
        if (config.version) {
            changeStreamSpec.version = config.version;
        }
        if (config.ignoreRemovedShards) {
            changeStreamSpec.ignoreRemovedShards = true;
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

        // TODO SERVER-107688: drop leaked tmp.renameCollection events from cross-DB renames.
        // Remove once the server marks those internal writes fromMigrate.
        if (config.watchMode === ChangeStreamWatchMode.kCluster) {
            pipeline.push({$match: {"ns.coll": {$not: /^tmp[A-Za-z0-9]+\.renameCollection$/}}});
        }

        const cursorOptions = {};
        if (config.batchSize !== undefined) {
            cursorOptions.batchSize = config.batchSize;
        }

        const watchOptions = {
            pipeline: pipeline,
            collection:
                config.watchMode === ChangeStreamWatchMode.kCollection ? config.collName : 1,
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
                    ({cst, cursor} = ChangeStreamReader._openChangeStream(
                        conn,
                        cfg,
                        resumeToken,
                        wasInvalidate,
                    ));
                }
                // Always use skipFirst=false to check the current batch before issuing getMore.
                // This ensures we don't miss events in firstBatch (after open) or nextBatch.
                const changeEvent = cst.getNextChanges(cursor, 1, false)[0];
                result = {changeEvent, cst, cursor};
                return true;
            } catch (e) {
                if (
                    !TestData.enableBgMutator ||
                    !ChangeStreamReader.kFCVRetryableErrors.includes(e.code)
                ) {
                    throw e;
                }
                jsTest.log.debug("ChangeStreamReader FCV error, will retry", {
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
    static _processEvent(conn, cfg, changeEvent, count, readEventTypes, extraFields = {}) {
        assert(changeEvent, `Expected change event at index ${count}, but got none`);
        assert(changeEvent._id, `Change event at index ${count} missing _id (resume token)`);

        const isInvalidate = isInvalidated(changeEvent);
        readEventTypes.push(changeEvent.operationType);

        jsTest.log.debug("ChangeStreamReader Read event", {
            instanceName: cfg.instanceName,
            eventIndex: count + 1,
            total: cfg.numberOfEventsToRead,
            operationType: changeEvent.operationType,
        });

        // cursorClosed is true for invalidate events (server closes cursor after invalidate).
        Connector.writeChangeEvent(conn, cfg.instanceName, {
            changeEvent,
            cursorClosed: isInvalidate,
            ...extraFields,
        });

        return isInvalidate;
    }

    /**
     * The single entry point through which raw change events enter the reader.
     *
     * Fetches at most one event (blocking when requested) and runs deduplication. The
     * reader's resume token is ALWAYS advanced here — including for duplicate command (DDL)
     * events suppressed by dedup — so a reopened cursor never re-reads a suppressed event.
     * This invariant lives nowhere else.
     *
     * Duplicates are collapsed internally (the fetch loops until a non-duplicate event or
     * exhaustion) unless `allowDuplicates` is set: only the fetch-one-and-resume mode opts
     * in, because its fresh-cursor-per-event policy must close the cursor that carried a
     * duplicate before fetching the next one.
     *
     * @param {Object} st - Reader state (see _execute).
     * @param {boolean} [options.blocking] - Block until an event arrives (continuous
     *     and fetch-one-and-resume modes) instead of returning null on an empty batch.
     * @param {boolean} [options.allowDuplicates] - Return collapsed duplicates to the
     *     caller (marked `duplicate: true`) instead of skipping them internally.
     * @returns {{event: Object|null, duplicate: boolean}} `event` is null only when no
     *     event was available (non-blocking reads); `duplicate` marks a suppressed
     *     duplicate whose resume token was still advanced.
     * @private
     */
    static _nextEvent(st, {blocking = false, allowDuplicates = false} = {}) {
        while (true) {
            let event;
            if (blocking) {
                ({
                    changeEvent: event,
                    cst: st.cst,
                    cursor: st.cursor,
                } = ChangeStreamReader._readOneEvent(
                    st.conn,
                    st.cfg,
                    st.cst,
                    st.cursor,
                    st.resumeToken,
                    st.useStartAfter,
                ));
            } else {
                event = ChangeStreamReader._tryGetNextEvent(st.cst, st.cursor);
            }
            if (event === null) {
                return {event: null, duplicate: false};
            }

            // Always advance the resume token, even for a suppressed duplicate, so the
            // reopened cursor does not re-read it.
            st.resumeToken = event._id;

            if (st.dedup.shouldSkip(event)) {
                jsTest.log.debug("ChangeStreamReader collapsed duplicate event", {
                    instanceName: st.cfg.instanceName,
                    operationType: event.operationType,
                    ns: event.ns,
                });
                if (allowDuplicates) {
                    return {event, duplicate: true};
                }
                continue;
            }
            return {event, duplicate: false};
        }
    }

    /**
     * Read events continuously, keeping the cursor open.
     * Handles invalidate events by reopening the cursor with startAfter.
     * @param {Object} st - Reader state (see _execute).
     * @private
     */
    static _readContinuous(st) {
        const cfg = st.cfg;
        jsTest.log.debug("ChangeStreamReader Starting continuous read", cfg);
        const readEventTypes = [];

        // Read until numberOfEventsToRead events have been RECORDED. Duplicate command
        // (DDL) events keep advancing the resume token but are neither recorded nor counted.
        let count = 0;
        while (count < cfg.numberOfEventsToRead) {
            const {event} = ChangeStreamReader._nextEvent(st, {blocking: true});

            const isInvalidate = ChangeStreamReader._processEvent(
                st.conn,
                cfg,
                event,
                count,
                readEventTypes,
            );
            st.useStartAfter = isInvalidate;
            count++;

            if (isInvalidate) {
                tryCleanUp(st.cst, cfg.instanceName);
                st.cst = null;
                st.cursor = null;
            }
        }

        jsTest.log.debug("ChangeStreamReader Read events", {
            instanceName: cfg.instanceName,
            readEventTypes,
        });
        tryCleanUp(st.cst, cfg.instanceName);
    }

    /**
     * Read all available events until a getMore returns no events (awaitData
     * timeout). Writers must be finished before calling this method.
     *
     * @param {Object} st - Reader state (see _execute).
     * @param {boolean} reopenPerEvent - If true, reopen the cursor after every
     *   event (FOAR semantics). If false, keep it open and only reopen after
     *   invalidates (continuous semantics).
     * @private
     */
    static _drainEvents(st, reopenPerEvent) {
        const cfg = st.cfg;
        jsTest.log.debug(
            `ChangeStreamReader Starting drain (reopenPerEvent=${reopenPerEvent})`,
            cfg,
        );

        const readEventTypes = [];
        let count = 0;

        ({cst: st.cst, cursor: st.cursor} = ChangeStreamReader._openChangeStream(
            st.conn,
            cfg,
            null,
            false,
        ));

        while (true) {
            const {event} = ChangeStreamReader._nextEvent(st);
            if (event === null) {
                break;
            }

            const isInvalidate = ChangeStreamReader._processEvent(
                st.conn,
                cfg,
                event,
                count++,
                readEventTypes,
            );

            if (reopenPerEvent || isInvalidate) {
                tryCleanUp(st.cst, cfg.instanceName);
                ({cst: st.cst, cursor: st.cursor} = ChangeStreamReader._openChangeStream(
                    st.conn,
                    cfg,
                    st.resumeToken,
                    isInvalidate,
                ));
            }
        }

        tryCleanUp(st.cst, cfg.instanceName);
        jsTest.log.debug("ChangeStreamReader Read events", {
            instanceName: cfg.instanceName,
            readEventTypes,
        });
    }

    /**
     * Read events one at a time, closing and reopening the cursor after each event.
     * Uses resumeAfter with the previous event's token.
     * After an invalidate, uses startAfter to reopen the cursor.
     *
     * '_readOneEvent' (via _nextEvent with blocking=true) blocks until an event arrives, so it
     * never sees an empty batch and can only resume from an event token. With 'resumeFromPbrt'
     * the cursor is kept open for up to kMaxPbrtGetMores more getMores; the first empty batch
     * yields a postBatchResumeToken, which is then used to resume, as drivers do on an idle
     * stream.
     *
     * @param {Object} st - Reader state (see _execute).
     * @param {boolean} resumeFromPbrt - Also poll the open cursor after each event and
     *   resume from its post-batch resume token when the batch is empty.
     * @private
     */
    static _readFetchOneAndResume(st, resumeFromPbrt = false) {
        const cfg = st.cfg;
        jsTest.log.debug(
            `ChangeStreamReader Starting fetch-one-and-resume (resumeFromPbrt=${resumeFromPbrt})`,
            cfg,
        );
        // null, "eventToken" or "pbrt": how the current cursor was opened.
        let resumedFrom = null;
        const readEventTypes = [];
        const resumeStats = {pbrt: 0, eventToken: 0};

        let count = 0;
        while (count < cfg.numberOfEventsToRead) {
            // Fetch-one-and-resume opens a fresh cursor for every event. It opts into
            // seeing duplicates: a collapsed duplicate must close this fresh cursor so
            // the next event is fetched from a cursor resumed past the duplicate.
            st.cst = null;
            st.cursor = null;
            const {event, duplicate} = ChangeStreamReader._nextEvent(st, {
                blocking: true,
                allowDuplicates: true,
            });
            if (duplicate) {
                tryCleanUp(st.cst, cfg.instanceName);
                continue;
            }

            let isInvalidate = ChangeStreamReader._processEvent(
                st.conn,
                cfg,
                event,
                count++,
                readEventTypes,
                {resumedFrom},
            );
            resumedFrom = "eventToken";

            // An invalidate closes the cursor, so there is nothing left to poll.
            if (resumeFromPbrt && !isInvalidate) {
                for (
                    let i = 0;
                    i < ChangeStreamReader.kMaxPbrtGetMores && count < cfg.numberOfEventsToRead;
                    i++
                ) {
                    const {event: extraEvent} = ChangeStreamReader._nextEvent(st);
                    if (extraEvent === null) {
                        st.resumeToken = st.cst.getResumeToken(st.cursor);
                        resumedFrom = "pbrt";
                        break;
                    }
                    isInvalidate = ChangeStreamReader._processEvent(
                        st.conn,
                        cfg,
                        extraEvent,
                        count++,
                        readEventTypes,
                    );
                    if (isInvalidate) {
                        break;
                    }
                }
            }
            resumeStats[resumedFrom]++;
            st.useStartAfter = isInvalidate;

            tryCleanUp(st.cst, cfg.instanceName);
        }

        jsTest.log.info("ChangeStreamReader Read events", {
            instanceName: cfg.instanceName,
            resumeStats,
            readEventTypes,
        });
    }

    /** Max getMores per cursor when looking for an empty batch (see resumeFromPbrt). */
    static kMaxPbrtGetMores = 3;

    /**
     * Issues one getMore (batchSize 1) and returns the event, or null on an empty batch.
     * Does not block or retry. The cursor is updated in place.
     * @private
     */
    static _tryGetNextEvent(cst, cursor) {
        cst.getNextBatch(cursor);
        const batch = cursor.nextBatch || [];
        assert.lte(batch.length, 1, "expected at most one event per batch", {batch});
        return batch.length === 0 ? null : batch[0];
    }
}

export {ChangeStreamReader, ChangeStreamReadingMode};
