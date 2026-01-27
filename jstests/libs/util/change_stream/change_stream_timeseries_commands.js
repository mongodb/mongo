import {Command} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";

/**
 * Command that creates a time-series collection with the given time/meta fields.
 */
export class CreateTimeseriesCollectionCommand extends Command {
    constructor({dbName, collName, timeField, metaField}) {
        super(dbName, collName, /* shardSet */ null, /* collectionCtx */ {});
        this.timeField = timeField;
        this.metaField = metaField;
    }

    execute(conn) {
        const db = conn.getDB(this.dbName);
        const tsOptions = {timeseries: {timeField: this.timeField}};
        if (this.metaField) {
            tsOptions.timeseries.metaField = this.metaField;
        }
        assertCreateCollection(db, this.collName, tsOptions);
    }

    getChangeEvents(ctx) {
        const {watchMode, watchedNss, showExpandedEvents} = ctx;
        if (!showExpandedEvents) {
            return [];
        }

        const createEvent = {
            operationType: "create",
            ns: {db: this.dbName, coll: this.collName},
            nsType: "timeseries",
        };

        if (
            watchMode === ChangeStreamWatchMode.kCollection &&
            watchedNss &&
            watchedNss.db === this.dbName &&
            watchedNss.coll === this.collName
        ) {
            return [createEvent];
        }

        if (watchMode === ChangeStreamWatchMode.kDb && watchedNss && watchedNss.db === this.dbName) {
            return [createEvent];
        }

        if (watchMode === ChangeStreamWatchMode.kCluster) {
            return [createEvent];
        }

        return [];
    }
}

/**
 * Command that inserts a measurement into a time-series collection and
 * knows which bucket-style change stream event that should produce.
 *
 * It does *not* try to compute bucket documents; you pass the expected bucket
 * fullDocument explicitly.
 */
export class TimeseriesInsertCommand extends Command {
    /**
     * @param {Object} params
     * @param {{db: string, coll: string}} params.insertNss
     *     Namespace where we actually run the insert (the time-series collection).
     * @param {{db: string, coll: string}} params.eventNss
     *     Namespace under which the change stream should report this insert
     *     (can be buckets or regular collection name, depending on FCV/viewless behavior).
     * @param {Object} params.insertDoc
     *     Measurement document to insert (e.g. {_id: 1, t: date1, m: "preUpgrade", v: "preUpgrade"}).
     * @param {Object} params.expectedFullDocument
     *     Expected bucket document for change stream `fullDocument` (hardcoded).
     * @param {boolean} [params.requiresRawData=false]
     *     If true, this event is only visible when ctx.rawData === true.
     */
    constructor({insertNss, eventNss, insertDoc, expectedFullDocument, requiresRawData = false}) {
        super(insertNss.db, insertNss.coll, /* shardSet */ null, /* collectionCtx */ {});
        this.insertNss = insertNss;
        this.eventNss = eventNss;
        this.insertDoc = insertDoc;
        this.expectedFullDocument = expectedFullDocument;
        this.requiresRawData = requiresRawData;
    }

    execute(conn) {
        const coll = conn.getDB(this.insertNss.db).getCollection(this.insertNss.coll);
        assert.commandWorked(coll.insert(this.insertDoc));
    }

    getChangeEvents(ctx) {
        const {watchMode, watchedNss, showSystemEvents, rawData} = ctx;

        // Respect rawData gating, e.g. inserts performed in 9.0 FCV require rawData to be set, for them to be visible.
        if (this.requiresRawData && !rawData) {
            return [];
        }

        const isSystemBuckets = this.eventNss.coll && this.eventNss.coll.startsWith("system.buckets.");
        if (isSystemBuckets && !showSystemEvents) {
            return [];
        }

        const insertEvent = {
            operationType: "insert",
            ns: this.eventNss,
            fullDocument: this.expectedFullDocument,
        };

        if (
            watchMode === ChangeStreamWatchMode.kCollection &&
            watchedNss.db === this.eventNss.db &&
            watchedNss.coll === this.eventNss.coll
        ) {
            return [insertEvent];
        }

        if (watchMode === ChangeStreamWatchMode.kDb && watchedNss.db === this.eventNss.db) {
            return [insertEvent];
        }

        if (watchMode === ChangeStreamWatchMode.kCluster) {
            return [insertEvent];
        }

        return [];
    }
}

export class FCVUpgradeCommand extends Command {
    constructor({toVersion, timeseriesCollections}) {
        super(/* dbName */ null, /* collName */ null, /* shardSet */ null, /* collectionCtx */ {});
        this.toVersion = toVersion;
        this.timeseriesCollections = timeseriesCollections;
    }

    execute(conn) {
        const adminDB = conn.getDB("admin");
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: this.toVersion, confirm: true}));
    }

    getChangeEvents(ctx) {
        const {watchMode, watchedNss} = ctx;
        const events = [];

        for (const {regularNss, bucketsNss} of this.timeseriesCollections) {
            const renameEvent = {
                operationType: "rename",
                ns: bucketsNss,
                to: regularNss,
            };

            if (watchMode === ChangeStreamWatchMode.kCollection) {
                // Only streams watching the *source* collection (buckets) see rename+invalidate.
                if (watchedNss && watchedNss.db === bucketsNss.db && watchedNss.coll === bucketsNss.coll) {
                    events.push(renameEvent);
                    events.push({operationType: "invalidate"});
                    break;
                }
            } else if (watchMode === ChangeStreamWatchMode.kDb) {
                if (watchedNss && watchedNss.db === bucketsNss.db) {
                    events.push(renameEvent);
                }
            } else if (watchMode === ChangeStreamWatchMode.kCluster) {
                events.push(renameEvent);
            }
        }

        return events;
    }
}

export class FCVDowngradeCommand extends Command {
    constructor({toVersion, timeseriesCollections}) {
        super(/* dbName */ null, /* collName */ null, /* shardSet */ null, /* collectionCtx */ {});
        this.toVersion = toVersion;
        this.timeseriesCollections = timeseriesCollections;
    }

    execute(conn) {
        const adminDB = conn.getDB("admin");
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: this.toVersion, confirm: true}));
    }

    getChangeEvents(ctx) {
        const {watchMode, watchedNss} = ctx;
        const events = [];

        for (const {regularNss, bucketsNss} of this.timeseriesCollections) {
            const renameEvent = {
                operationType: "rename",
                ns: regularNss,
                to: bucketsNss,
            };

            if (watchMode === ChangeStreamWatchMode.kCollection) {
                // Only streams watching the *source* collection (regular/viewless) see rename+invalidate.
                if (watchedNss && watchedNss.db === regularNss.db && watchedNss.coll === regularNss.coll) {
                    events.push(renameEvent);
                    events.push({operationType: "invalidate"});
                    break;
                }
            } else if (watchMode === ChangeStreamWatchMode.kDb) {
                if (watchedNss && watchedNss.db === regularNss.db) {
                    events.push(renameEvent);
                }
            } else if (watchMode === ChangeStreamWatchMode.kCluster) {
                events.push(renameEvent);
            }
        }

        return events;
    }
}
