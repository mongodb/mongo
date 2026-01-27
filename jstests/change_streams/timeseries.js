/**
 * Basic test to make sure events from timeseries buckets collections look normal and don't get
 * filtered.
 * @tags: [
 *     change_stream_does_not_expect_txns,
 *     assumes_unsharded_collection,
 *     # The test runs the change streams over the database and the whole cluster.
 *     do_not_run_in_whole_db_passthrough,
 *     do_not_run_in_whole_cluster_passthrough,
 *     # Requires change stream rawData support introduced in FCV 8.3.
 *     requires_fcv_83,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ChangeStreamTest, getClusterTime, getNextClusterTime} from "jstests/libs/query/change_stream_util.js";
import {describe, before, it} from "jstests/libs/mochalite.js";
import {getRawOperationSpec, isRawOperationSupported} from "jstests/libs/raw_operation_utils.js";
import {assertCreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

describe("$changeStream", function () {
    const dbName = "db";
    const collName = "coll";

    const bucketsCollName = "system.buckets." + collName;
    const timeseriesCollNs = {"db": dbName, "coll": collName};
    const bucketsCollNs = {"db": dbName, "coll": bucketsCollName};

    const legacyTimeseriesEventMap = {
        bucketsCreateCollectionEvent: {
            "operationType": "create",
            "ns": bucketsCollNs,
            "operationDescription": {
                "validator": {
                    "$jsonSchema": {
                        "bsonType": "object",
                        "required": ["_id", "control", "data"],
                        "properties": {
                            "_id": {"bsonType": "objectId"},
                            "control": {
                                "bsonType": "object",
                                "required": ["version", "min", "max"],
                                "properties": {
                                    "version": {"bsonType": "number"},
                                    "min": {
                                        "bsonType": "object",
                                        "required": ["ts"],
                                        "properties": {"ts": {"bsonType": "date"}},
                                    },
                                    "max": {
                                        "bsonType": "object",
                                        "required": ["ts"],
                                        "properties": {"ts": {"bsonType": "date"}},
                                    },
                                    "closed": {"bsonType": "bool"},
                                    "count": {"bsonType": "number", "minimum": 1},
                                },
                                "additionalProperties": false,
                            },
                            "data": {"bsonType": "object"},
                            "meta": {},
                        },
                        "additionalProperties": false,
                    },
                },
                "clusteredIndex": true,
                "timeseries": {
                    "timeField": "ts",
                    "metaField": "meta",
                    "granularity": "seconds",
                    "bucketMaxSpanSeconds": 3600,
                },
            },
            "nsType": "collection",
        },
        metaIndexCreationEvent: {
            "operationType": "createIndexes",
            "ns": bucketsCollNs,
            "operationDescription": {
                "indexes": [
                    {
                        "v": 2,
                        "name": "meta_1_ts_1",
                        "key": {"meta": 1, "control.min.ts": 1, "control.max.ts": 1},
                    },
                ],
            },
        },
        viewCollectionCreationEvent: {
            "operationType": "create",
            "ns": {"db": dbName, "coll": "system.views"},
            "operationDescription": {"idIndex": {"v": 2, "key": {"_id": 1}, "name": "_id_"}},
            "nsType": "collection",
        },
        viewCreationEvent: {
            "operationType": "create",
            "ns": timeseriesCollNs,
            "operationDescription": {
                "viewOn": bucketsCollName,
                "pipeline": [
                    {
                        "$_internalUnpackBucket": {
                            "timeField": "ts",
                            "metaField": "meta",
                            "bucketMaxSpanSeconds": 3600,
                        },
                    },
                ],
            },
            "nsType": "timeseries",
        },
        dropMeIndexCreationEvent: {
            "operationType": "createIndexes",
            "ns": bucketsCollNs,
            "operationDescription": {
                "indexes": [
                    {
                        "v": 2,
                        "key": {"control.min.ts": 1, "control.max.ts": 1, "meta.b": 1},
                        "name": "dropMe",
                    },
                ],
            },
        },
        insertEvent: {
            "operationType": "insert",
            "fullDocument": {
                "control": {
                    "count": 1,
                    "max": {"_id": 1, "ts": ISODate("1970-01-01T00:00:01Z")},
                    "min": {
                        "_id": 1,
                        "ts": ISODate("1970-01-01T00:00:00Z"),
                    },
                    "version": TimeseriesTest.BucketVersion.kCompressedSorted,
                },
                "data": {"ts": BinData(7, "CQDoAwAAAAAAAAA="), "_id": BinData(7, "AQAAAAAAAADwPwA=")},
                "meta": {"a": 1},
            },
            "ns": bucketsCollNs,
        },
        updateEvent: {
            "operationType": "update",
            "ns": bucketsCollNs,
            "updateDescription": {
                "updatedFields": {
                    "control.count": 2,
                    "data.ts": {"o": 10, "d": BinData(0, "gA4AAAAAAAAAAA==")},
                    "data._id": {"o": 10, "d": BinData(0, "kA4AAAAAAAAAAA==")},
                },
                "removedFields": [],
                "truncatedArrays": [],
                "disambiguatedPaths": {},
            },
        },
        insertApplyOpsEvent1: {
            "operationType": "insert",
            "fullDocument": {
                "control": {
                    "version": TimeseriesTest.BucketVersion.kCompressedSorted,
                    "min": {
                        "_id": 10,
                        "ts": ISODate("2024-05-01T00:00:00Z"),
                        value: 3,
                    },
                    "max": {
                        "_id": 10,
                        "ts": ISODate("2024-05-01T00:00:00Z"),
                        value: 3,
                    },
                    "count": 1,
                },
                "meta": "0",
                "data": {
                    "ts": BinData(7, "CQAAcHMxjwEAAAA="),
                    "_id": BinData(7, "AQAAAAAAAAAkQAA="),
                    "value": BinData(7, "AQAAAAAAAAAIQAA="),
                },
            },
            "ns": bucketsCollNs,
        },
        insertApplyOpsEvent2: {
            "operationType": "insert",
            "fullDocument": {
                "control": {
                    "version": TimeseriesTest.BucketVersion.kCompressedSorted,
                    "min": {
                        "_id": 11,
                        "ts": ISODate("2024-05-01T00:01:00Z"),
                        value: 4,
                    },
                    "max": {
                        "_id": 11,
                        "ts": ISODate("2024-05-01T00:01:00Z"),
                        value: 4,
                    },
                    "count": 1,
                },
                "meta": "1",
                "data": {
                    "ts": BinData(7, "CQBgWnQxjwEAAAA="),
                    "_id": BinData(7, "AQAAAAAAAAAmQAA="),
                    "value": BinData(7, "AQAAAAAAAAAQQAA="),
                },
            },
            "ns": bucketsCollNs,
        },
        insertApplyOpsEvent3: {
            "operationType": "insert",
            "fullDocument": {
                "control": {
                    "version": TimeseriesTest.BucketVersion.kCompressedSorted,
                    "min": {
                        "_id": 12,
                        "ts": ISODate("2024-05-01T00:02:00Z"),
                        value: 5,
                    },
                    "max": {
                        "_id": 12,
                        "ts": ISODate("2024-05-01T00:02:00Z"),
                        value: 5,
                    },
                    "count": 1,
                },
                "meta": "2",
                "data": {
                    "ts": BinData(7, "CQDARHUxjwEAAAA="),
                    "_id": BinData(7, "AQAAAAAAAAAoQAA="),
                    "value": BinData(7, "AQAAAAAAAAAUQAA="),
                },
            },
            "ns": bucketsCollNs,
        },
        deleteEvent: {"operationType": "delete", "ns": bucketsCollNs},
        viewHourGranularityModificationEvent: {
            "operationType": "modify",
            "ns": timeseriesCollNs,
            "operationDescription": {
                "viewOn": bucketsCollName,
                "pipeline": [
                    {
                        "$_internalUnpackBucket": {
                            "timeField": "ts",
                            "metaField": "meta",
                            "bucketMaxSpanSeconds": 2592000,
                        },
                    },
                ],
            },
        },
        bucketsCollectionHourGranularityModificationEvent: {
            "operationType": "modify",
            "ns": bucketsCollNs,
            "operationDescription": {"timeseries": {"granularity": "hours"}},
            "stateBeforeChange": {
                "collectionOptions": {
                    "validator": {
                        "$jsonSchema": {
                            "bsonType": "object",
                            "required": ["_id", "control", "data"],
                            "properties": {
                                "_id": {"bsonType": "objectId"},
                                "control": {
                                    "bsonType": "object",
                                    "required": ["version", "min", "max"],
                                    "properties": {
                                        "version": {"bsonType": "number"},
                                        "min": {
                                            "bsonType": "object",
                                            "required": ["ts"],
                                            "properties": {"ts": {"bsonType": "date"}},
                                        },
                                        "max": {
                                            "bsonType": "object",
                                            "required": ["ts"],
                                            "properties": {"ts": {"bsonType": "date"}},
                                        },
                                        "closed": {"bsonType": "bool"},
                                        "count": {"bsonType": "number", "minimum": 1},
                                    },
                                    "additionalProperties": false,
                                },
                                "data": {"bsonType": "object"},
                                "meta": {},
                            },
                            "additionalProperties": false,
                        },
                    },
                    "clusteredIndex": true,
                    "timeseries": {
                        "timeField": "ts",
                        "metaField": "meta",
                        "granularity": "seconds",
                        "bucketMaxSpanSeconds": 3600,
                    },
                },
            },
        },
        bucketsCollectionExpirationModificationEvent: {
            "operationType": "modify",
            "ns": bucketsCollNs,
            "operationDescription": {"expireAfterSeconds": NumberLong(1)},
            "stateBeforeChange": {
                "collectionOptions": {
                    "validator": {
                        "$jsonSchema": {
                            "bsonType": "object",
                            "required": ["_id", "control", "data"],
                            "properties": {
                                "_id": {"bsonType": "objectId"},
                                "control": {
                                    "bsonType": "object",
                                    "required": ["version", "min", "max"],
                                    "properties": {
                                        "version": {"bsonType": "number"},
                                        "min": {
                                            "bsonType": "object",
                                            "required": ["ts"],
                                            "properties": {"ts": {"bsonType": "date"}},
                                        },
                                        "max": {
                                            "bsonType": "object",
                                            "required": ["ts"],
                                            "properties": {"ts": {"bsonType": "date"}},
                                        },
                                        "closed": {"bsonType": "bool"},
                                        "count": {"bsonType": "number", "minimum": 1},
                                    },
                                    "additionalProperties": false,
                                },
                                "data": {"bsonType": "object"},
                                "meta": {},
                            },
                            "additionalProperties": false,
                        },
                    },
                    "clusteredIndex": true,
                    "timeseries": {
                        "timeField": "ts",
                        "metaField": "meta",
                        "granularity": "hours",
                        "bucketMaxSpanSeconds": 2592000,
                    },
                },
            },
        },
        dropMeIndexDropEvent: {
            "operationType": "dropIndexes",
            "ns": bucketsCollNs,
            "operationDescription": {
                "indexes": [
                    {
                        "v": 2,
                        "key": {"control.min.ts": 1, "control.max.ts": 1, "meta.b": 1},
                        "name": "dropMe",
                    },
                ],
            },
        },
        viewDropEvent: {"operationType": "drop", "ns": timeseriesCollNs},
        bucketsCollectionDropEvent: {"operationType": "drop", "ns": bucketsCollNs},
    };

    const viewlessTimeseriesEventMap = {
        collectionCreationEvent: {
            "operationType": "create",
            "ns": timeseriesCollNs,
            "operationDescription": {
                "clusteredIndex": true,
                "timeseries": {
                    "timeField": "ts",
                    "metaField": "meta",
                    "granularity": "seconds",
                    "bucketMaxSpanSeconds": 3600,
                },
            },
            "nsType": "timeseries",
        },
        metaIndexCreationEvent: {
            "operationType": "createIndexes",
            "ns": timeseriesCollNs,
            "operationDescription": {
                "indexes": [
                    {
                        "v": 2,
                        "key": {"meta": 1, "control.min.ts": 1, "control.max.ts": 1},
                        "name": "meta_1_ts_1",
                    },
                ],
            },
        },
        dropMeIndexCreationEvent: {
            "operationType": "createIndexes",
            "ns": timeseriesCollNs,
            "operationDescription": {
                "indexes": [
                    {
                        "v": 2,
                        "key": {"control.min.ts": 1, "control.max.ts": 1, "meta.b": 1},
                        "name": "dropMe",
                    },
                ],
            },
        },
        insertEvent: {
            "operationType": "insert",
            "fullDocument": {
                "control": {
                    "count": 1,
                    "max": {"_id": 1, "ts": ISODate("1970-01-01T00:00:01Z")},
                    "min": {
                        "_id": 1,
                        "ts": ISODate("1970-01-01T00:00:00Z"),
                    },
                    "version": TimeseriesTest.BucketVersion.kCompressedSorted,
                },
                "data": {"ts": BinData(7, "CQDoAwAAAAAAAAA="), "_id": BinData(7, "AQAAAAAAAADwPwA=")},
                "meta": {"a": 1},
            },
            "ns": timeseriesCollNs,
        },
        updateEvent: {
            "operationType": "update",
            "updateDescription": {
                "updatedFields": {
                    "control.count": 2,
                    "data.ts": {"o": 10, "d": BinData(0, "gA4AAAAAAAAAAA==")},
                    "data._id": {"o": 10, "d": BinData(0, "kA4AAAAAAAAAAA==")},
                },
                "removedFields": [],
                "truncatedArrays": [],
                "disambiguatedPaths": {},
            },
            "ns": timeseriesCollNs,
        },
        insertApplyOpsEvent1: {
            "operationType": "insert",
            "fullDocument": {
                "control": {
                    "version": TimeseriesTest.BucketVersion.kCompressedSorted,
                    "min": {
                        "_id": 10,
                        "ts": ISODate("2024-05-01T00:00:00Z"),
                        value: 3,
                    },
                    "max": {
                        "_id": 10,
                        "ts": ISODate("2024-05-01T00:00:00Z"),
                        value: 3,
                    },
                    "count": 1,
                },
                "meta": "0",
                "data": {
                    "ts": BinData(7, "CQAAcHMxjwEAAAA="),
                    "_id": BinData(7, "AQAAAAAAAAAkQAA="),
                    "value": BinData(7, "AQAAAAAAAAAIQAA="),
                },
            },
            "ns": timeseriesCollNs,
        },
        insertApplyOpsEvent2: {
            "operationType": "insert",
            "fullDocument": {
                "control": {
                    "version": TimeseriesTest.BucketVersion.kCompressedSorted,
                    "min": {
                        "_id": 11,
                        "ts": ISODate("2024-05-01T00:01:00Z"),
                        value: 4,
                    },
                    "max": {
                        "_id": 11,
                        "ts": ISODate("2024-05-01T00:01:00Z"),
                        value: 4,
                    },
                    "count": 1,
                },
                "meta": "1",
                "data": {
                    "ts": BinData(7, "CQBgWnQxjwEAAAA="),
                    "_id": BinData(7, "AQAAAAAAAAAmQAA="),
                    "value": BinData(7, "AQAAAAAAAAAQQAA="),
                },
            },
            "ns": timeseriesCollNs,
        },
        insertApplyOpsEvent3: {
            "operationType": "insert",
            "fullDocument": {
                "control": {
                    "version": TimeseriesTest.BucketVersion.kCompressedSorted,
                    "min": {
                        "_id": 12,
                        "ts": ISODate("2024-05-01T00:02:00Z"),
                        value: 5,
                    },
                    "max": {
                        "_id": 12,
                        "ts": ISODate("2024-05-01T00:02:00Z"),
                        value: 5,
                    },
                    "count": 1,
                },
                "meta": "2",
                "data": {
                    "ts": BinData(7, "CQDARHUxjwEAAAA="),
                    "_id": BinData(7, "AQAAAAAAAAAoQAA="),
                    "value": BinData(7, "AQAAAAAAAAAUQAA="),
                },
            },
            "ns": timeseriesCollNs,
        },
        deleteEvent: {"operationType": "delete", "ns": timeseriesCollNs},
        collectionHourGranularityModificationEvent: {
            "operationType": "modify",
            "ns": timeseriesCollNs,
            "operationDescription": {"timeseries": {"granularity": "hours"}},
            "stateBeforeChange": {
                "collectionOptions": {
                    "clusteredIndex": true,
                    "timeseries": {
                        "timeField": "ts",
                        "metaField": "meta",
                        "granularity": "seconds",
                        "bucketMaxSpanSeconds": 3600,
                    },
                },
            },
        },
        collectionExpirationModificationEvent: {
            "operationType": "modify",
            "ns": timeseriesCollNs,
            "operationDescription": {"expireAfterSeconds": NumberLong(1)},
            "stateBeforeChange": {
                "collectionOptions": {
                    "clusteredIndex": true,
                    "timeseries": {
                        "timeField": "ts",
                        "metaField": "meta",
                        "granularity": "hours",
                        "bucketMaxSpanSeconds": 2592000,
                    },
                },
            },
        },
        dropMeIndexDropEvent: {
            "operationType": "dropIndexes",
            "ns": timeseriesCollNs,
            "operationDescription": {
                "indexes": [
                    {
                        "v": 2,
                        "key": {"control.min.ts": 1, "control.max.ts": 1, "meta.b": 1},
                        "name": "dropMe",
                    },
                ],
            },
        },
        collectionDropEvent: {
            "operationType": "drop",
            "ns": timeseriesCollNs,
        },
    };

    function generateEvents(db) {
        const clusterTimeBeforeGeneratingEvents = getNextClusterTime(getClusterTime(db));

        const coll = assertCreateCollection(db, collName, {timeseries: {timeField: "ts", metaField: "meta"}});
        coll.createIndex({ts: 1, "meta.b": 1}, {name: "dropMe"});

        coll.insertOne({_id: 1, ts: new Date(1000), meta: {a: 1}});
        coll.insertOne({_id: 1, ts: new Date(1000), meta: {a: 1}});

        // NOTE: perform another write that issues applyOps on the buckets collection.
        const nMeasurements = 3;
        const docsToInsert = Array.from({length: nMeasurements}, (_, i) => ({
            _id: i + 10,
            ts: ISODate(`2024-05-01T00:0${i}:00Z`),
            meta: i.toString(),
            value: i + nMeasurements,
        }));
        assert.commandWorked(coll.insertMany(docsToInsert));

        coll.remove({"meta.a": 1});

        assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: "hours"}}));

        assert.commandWorked(db.runCommand({collMod: collName, expireAfterSeconds: 1}));
        coll.dropIndex("dropMe");
        assertDropCollection(db, collName);

        return clusterTimeBeforeGeneratingEvents;
    }

    function getExpectedChangeEvents(db, showSystemEvents, rawData) {
        const isViewlessTimeseriesEnabled = FeatureFlagUtil.isPresentAndEnabled(
            db,
            "CreateViewlessTimeseriesCollections",
        );
        if (isViewlessTimeseriesEnabled) {
            // For viewless timeseries collections, the 'rawData' flag controls whether we see any DDL or DML events.
            if (!rawData) {
                return [];
            }

            // NOTE: the set of events observed on viewless timeseries collections is the same regardless of the 'showSystemEvents' value.
            const eventMap = viewlessTimeseriesEventMap;
            return [
                eventMap.collectionCreationEvent,
                eventMap.metaIndexCreationEvent,
                eventMap.dropMeIndexCreationEvent,
                eventMap.insertEvent,
                eventMap.updateEvent,
                eventMap.insertApplyOpsEvent1,
                eventMap.insertApplyOpsEvent2,
                eventMap.insertApplyOpsEvent3,
                eventMap.deleteEvent,
                eventMap.collectionHourGranularityModificationEvent,
                eventMap.collectionExpirationModificationEvent,
                eventMap.dropMeIndexDropEvent,
                eventMap.collectionDropEvent,
            ];
        } else {
            // NOTE: the set of events observed on bucketed timeseries does not depend on the 'rawData' value.
            const eventMap = legacyTimeseriesEventMap;
            if (showSystemEvents) {
                return [
                    eventMap.bucketsCreateCollectionEvent,
                    eventMap.metaIndexCreationEvent,
                    eventMap.viewCollectionCreationEvent,
                    eventMap.viewCreationEvent,
                    eventMap.dropMeIndexCreationEvent,
                    eventMap.insertEvent,
                    eventMap.updateEvent,
                    eventMap.insertApplyOpsEvent1,
                    eventMap.insertApplyOpsEvent2,
                    eventMap.insertApplyOpsEvent3,
                    eventMap.deleteEvent,
                    eventMap.viewHourGranularityModificationEvent,
                    eventMap.bucketsCollectionHourGranularityModificationEvent,
                    eventMap.bucketsCollectionExpirationModificationEvent,
                    eventMap.dropMeIndexDropEvent,
                    eventMap.viewDropEvent,
                    eventMap.bucketsCollectionDropEvent,
                ];
            } else {
                return [
                    eventMap.viewCreationEvent,
                    eventMap.viewHourGranularityModificationEvent,
                    eventMap.viewDropEvent,
                ];
            }
        }
    }

    before(function () {
        this.testDB = db.getSiblingDB(dbName);
        assert.commandWorked(this.testDB.dropDatabase());

        this.clusterTimeBeforeGeneratingEvents = generateEvents(this.testDB);
    });

    describe("when not specifying rawData", function () {
        it("should not allow change streams on time-series collections", function () {
            const db = this.testDB.getSiblingDB("reservedDB");
            const testColl = assertCreateCollection(db, collName, {timeseries: {timeField: "ts", metaField: "meta"}});
            testColl.insertOne({_id: 1, ts: new Date(), meta: {a: 1}});

            // Ensure that change streams are not allowed on time-series collections.
            // In v1 it fails immediately, while in v2 it fails when the cursor to the shard is opened.
            let response = db.runCommand({aggregate: collName, pipeline: [{$changeStream: {}}], cursor: {}});
            if (response.ok) {
                // In case we are running change streams version 2, the cursor may not be opened on the shard.
                // To ensure the failure indeed occurs, we issue a getMore command to ensure that the cursor
                // will be attempted to be opened on the shard and will fail.
                assert.eq(response._changeStreamVersion, "v2", "Change stream of version v1 should fail immediately");
                response = db.runCommand({getMore: response.cursor.id, collection: collName});
            }
            assert.commandFailedWithCode(response, [
                ErrorCodes.CommandNotSupportedOnView,
                ErrorCodes.CommandNotSupported,
            ]);
            assertDropCollection(db, collName);
        });

        for (const showSystemEvents of [true, false]) {
            const message = showSystemEvents ? "DDL events (and DML if not running in viewless ts)" : "DDL events";
            it(`should allow change stream on the database and should emit timeseries return ${message} with: showSystemEvents=${showSystemEvents}`, function () {
                const cst = new ChangeStreamTest(this.testDB);
                const cursor = cst.startWatchingChanges({
                    pipeline: [
                        {
                            $changeStream: {
                                showExpandedEvents: true,
                                showSystemEvents,
                                startAtOperationTime: this.clusterTimeBeforeGeneratingEvents,
                            },
                        },
                        {
                            $project: {
                                documentKey: 0,
                                "fullDocument._id": 0,
                                "stateBeforeChange.collectionOptions.uuid": 0,
                            },
                        },
                    ],
                    collection: 1,
                });

                const expectedChanges = getExpectedChangeEvents(db, showSystemEvents, false /* rawData */);
                cst.assertNextChangesEqual({cursor: cursor, expectedChanges});
                cst.assertNoChange(cursor);
            });

            it(`should allow change stream on the cluster and should emit timeseries return ${message} with: showSystemEvents=${showSystemEvents}`, function () {
                const cst = new ChangeStreamTest(this.testDB.getSiblingDB("admin"));
                const cursor = cst.startWatchingChanges({
                    pipeline: [
                        {
                            $changeStream: {
                                allChangesForCluster: true,
                                showExpandedEvents: true,
                                showSystemEvents,
                                startAtOperationTime: this.clusterTimeBeforeGeneratingEvents,
                            },
                        },
                        {
                            $match: {"ns.db": {"$ne": "reservedDB"}},
                        },
                        {
                            $project: {
                                documentKey: 0,
                                "fullDocument._id": 0,
                                "stateBeforeChange.collectionOptions.uuid": 0,
                            },
                        },
                    ],
                    collection: 1,
                });

                const expectedChanges = getExpectedChangeEvents(db, showSystemEvents, false /* rawData */);
                cst.assertNextChangesEqual({cursor: cursor, expectedChanges});
                cst.assertNoChange(cursor);
            });
        }
    });

    describe("when specifying rawData", function () {
        const allTimeseriesFlagsEnabled = [
            "CreateViewlessTimeseriesCollections",
            "MarkTimeseriesEventsInOplog",
            "RawDataCrudOperations",
        ]
            .map((ff) => FeatureFlagUtil.isPresentAndEnabled(db, ff))
            .every((v) => v);

        // NOTE: if ffs are not set, then the change stream will not deliver any events for timeseries collections.
        if (!(isRawOperationSupported(db) && allTimeseriesFlagsEnabled)) {
            jsTest.log.info("Can not run change stream timeseries tests as rawData flag is not supported");
            return;
        }

        for (const showSystemEvents of [true, false]) {
            const message = showSystemEvents ? "DDL events (and DML if not running in viewless ts)" : "DDL events";
            it(`should allow change stream on the collection and should emit timeseries return ${message} with: showSystemEvents=${showSystemEvents}`, function () {
                const cst = new ChangeStreamTest(this.testDB);
                const cursor = cst.startWatchingChanges({
                    pipeline: [
                        {
                            $changeStream: {
                                showExpandedEvents: true,
                                showSystemEvents,
                                startAtOperationTime: this.clusterTimeBeforeGeneratingEvents,
                            },
                        },
                        {
                            $project: {
                                documentKey: 0,
                                "fullDocument._id": 0,
                                "stateBeforeChange.collectionOptions.uuid": 0,
                            },
                        },
                    ],
                    collection: collName,
                    aggregateOptions: getRawOperationSpec(this.testDB),
                });

                const expectedChanges = getExpectedChangeEvents(db, showSystemEvents, true /* rawData */);
                const invalidateEvent = {"operationType": "invalidate"};
                expectedChanges.push(invalidateEvent);
                cst.assertNextChangesEqual({cursor: cursor, expectedChanges});
                cst.assertNoChange(cursor);
            });

            it(`should allow change stream on the database and should emit timeseries return ${message} with: showSystemEvents=${showSystemEvents}`, function () {
                const cst = new ChangeStreamTest(this.testDB);
                const cursor = cst.startWatchingChanges({
                    pipeline: [
                        {
                            $changeStream: {
                                showExpandedEvents: true,
                                showSystemEvents,
                                startAtOperationTime: this.clusterTimeBeforeGeneratingEvents,
                            },
                        },
                        {
                            $project: {
                                documentKey: 0,
                                "fullDocument._id": 0,
                                "stateBeforeChange.collectionOptions.uuid": 0,
                            },
                        },
                    ],
                    collection: 1,
                    aggregateOptions: getRawOperationSpec(this.testDB),
                });

                const expectedChanges = getExpectedChangeEvents(db, showSystemEvents, true /* rawData */);
                cst.assertNextChangesEqual({cursor: cursor, expectedChanges});
                cst.assertNoChange(cursor);
            });

            it(`should allow change stream on the cluster and should emit timeseries return ${message} with: showSystemEvents=${showSystemEvents}`, function () {
                const cst = new ChangeStreamTest(this.testDB.getSiblingDB("admin"));
                const cursor = cst.startWatchingChanges({
                    pipeline: [
                        {
                            $changeStream: {
                                allChangesForCluster: true,
                                showExpandedEvents: true,
                                showSystemEvents,
                                startAtOperationTime: this.clusterTimeBeforeGeneratingEvents,
                            },
                        },
                        {
                            $match: {"ns.db": {"$ne": "reservedDB"}},
                        },
                        {
                            $project: {
                                documentKey: 0,
                                "fullDocument._id": 0,
                                "stateBeforeChange.collectionOptions.uuid": 0,
                            },
                        },
                    ],
                    collection: 1,
                    aggregateOptions: getRawOperationSpec(this.testDB),
                });

                const expectedChanges = getExpectedChangeEvents(db, showSystemEvents, true /* rawData */);
                cst.assertNextChangesEqual({cursor: cursor, expectedChanges});
                cst.assertNoChange(cursor);
            });
        }
    });
});
