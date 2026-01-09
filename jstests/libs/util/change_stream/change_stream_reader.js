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

    /**
     * Run the change stream reader with the given configuration.
     * @param {Mongo} conn - MongoDB connection.
     * @param {Object} config - Configuration object containing:
     *   - instanceName: Name of the reader instance (used as collection name for storing events).
     *   - watchMode: ChangeStreamWatchMode value (kCollection, kDb, or kCluster).
     *   - dbName: Database name (required for Collection and Database types).
     *   - collName: Collection name (required for Collection type).
     *   - numberOfEventsToRead: Number of events to read before stopping.
     *   - readingMode: ChangeStreamReadingMode value. Default: Continuous.
     *   - showExpandedEvents: Optional boolean to show expanded events (default: false).
     *   - batchSize: Optional cursor batch size for getMore operations (default: 1).
     */
    static run(conn, config) {
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

        const changeStreamSpec = {};
        if (config.showExpandedEvents) {
            changeStreamSpec.showExpandedEvents = true;
        }
        if (config.watchMode === ChangeStreamWatchMode.kCluster) {
            changeStreamSpec.allChangesForCluster = true;
        }
        // Use startAfter for invalidate tokens, resumeAfter for normal tokens
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
        if (config.watchMode === ChangeStreamWatchMode.kCluster) {
            pipeline.push({$match: {"ns.db": {$ne: Connector.controlDatabase}}});
        }

        const watchOptions = {
            pipeline: pipeline,
            collection: config.watchMode === ChangeStreamWatchMode.kCollection ? config.collName : 1, // 1 means watch all collections
            aggregateOptions: {cursor: {batchSize: config.batchSize ?? ChangeStreamReader.kDefaultGetMoreBatchSize}},
        };

        const cursor = cst.startWatchingChanges(watchOptions);
        return {cst, cursor};
    }

    /**
     * Read events continuously, keeping the cursor open.
     * Handles invalidate events by reopening the cursor with startAfter.
     * @private
     */
    static _readContinuous(conn, cfg) {
        let {cst, cursor} = ChangeStreamReader._openChangeStream(conn, cfg);

        for (let count = 0; count < cfg.numberOfEventsToRead; count++) {
            // Always use skipFirst=false to check the current batch before issuing getMore.
            // This ensures we don't miss events in firstBatch (after open) or nextBatch.
            const changeEvent = cst.getNextChanges(cursor, 1, false /* skipFirst */)[0];

            assert(changeEvent, `Expected change event at index ${count}, but got none`);
            assert(changeEvent._id, `Change event at index ${count} missing _id (resume token)`);

            const isInvalidate = isInvalidated(changeEvent);

            // cursorClosed is true for invalidate events (server closes cursor after invalidate)
            Connector.writeChangeEvent(conn, cfg.instanceName, {
                changeEvent,
                cursorClosed: isInvalidate,
            });

            if (isInvalidate) {
                // Cursor is already killed by server after invalidate, no need to call cleanUp().
                // Must use startAfter (not resumeAfter) when resuming from invalidate.
                ({cst, cursor} = ChangeStreamReader._openChangeStream(conn, cfg, changeEvent._id, true));
            }
        }

        cst.cleanUp();
    }

    /**
     * Read events one at a time, closing and resuming the cursor after each event.
     * This mode tests resume token handling.
     * @private
     */
    static _readFetchOneAndResume(conn, cfg) {
        let resumeToken = null;
        let useStartAfter = false;

        for (let count = 0; count < cfg.numberOfEventsToRead; count++) {
            const {cst, cursor} = ChangeStreamReader._openChangeStream(conn, cfg, resumeToken, useStartAfter);

            // Use skipFirst=false to not ignore events that may be in firstBatch.
            // getOneChange() uses skipFirst=true which would drop events in firstBatch.
            const changeEvent = cst.getNextChanges(cursor, 1, false /* skipFirst */)[0];

            assert(changeEvent, `Expected change event at index ${count}, but got none`);
            assert(changeEvent._id, `Change event at index ${count} missing _id (resume token)`);

            const isInvalidate = isInvalidated(changeEvent);

            resumeToken = changeEvent._id;
            // Must use startAfter (not resumeAfter) when resuming from invalidate
            useStartAfter = isInvalidate;

            // cursorClosed is true for invalidate events (server closes cursor after invalidate)
            Connector.writeChangeEvent(conn, cfg.instanceName, {
                changeEvent,
                cursorClosed: isInvalidate,
            });

            cst.cleanUp();
        }
    }
}

export {ChangeStreamReader, ChangeStreamReadingMode};
