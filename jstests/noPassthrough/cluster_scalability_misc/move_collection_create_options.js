/*
 * Tests moveCollection and reshardCollection commands with all non-internal collection options.
 */
import {EncryptedClient} from "jstests/fle2/libs/encrypted_client_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function makeDocument(val) {
    return {_id: val, x: new Date(), y: val, z: NumberInt(val)};
}

function insertDocuments(conn, dbName, collName, {useBatch} = {
    useBatch: true
}) {
    const shouldEncrypt = conn.getAutoEncryptionOptions() !== undefined;
    const coll = conn.getDB(dbName).getCollection(collName);
    const docs = [];
    for (let i = 0; i < maxCount; i++) {
        const doc = makeDocument(i);
        if (!useBatch) {
            assert.commandWorked(shouldEncrypt ? coll.einsert(doc) : coll.insert(doc));
        } else {
            docs.push(doc);
        }
    }
    if (useBatch) {
        assert.commandWorked(shouldEncrypt ? coll.einsert(docs) : coll.insert(docs));
    }
}

function getDottedField(doc, fieldName) {
    let val = doc;
    const fieldNames = fieldName.split(".");
    for (let i = 0; i < fieldNames.length; i++) {
        val = val[fieldNames[i]];
    }
    return val;
}

function validateCollection(conn,
                            dbName,
                            collName,
                            shardKey,
                            {expectedCollOpts, expectedIndexes, expectNoShardingMetadata} = {}) {
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);

    // Validate collection options.
    const listCollectionsDoc = coll.exists();
    jsTest.log("*** Checking expectedCollOpts " + tojson({listCollectionsDoc, expectedCollOpts}));
    for (let fieldName in expectedCollOpts) {
        const actual = getDottedField(listCollectionsDoc.options, fieldName);
        const expected = expectedCollOpts[fieldName];
        assert.eq(bsonUnorderedFieldsCompare(actual, expected), 0, {fieldName, actual, expected});
    }
    assert.eq(coll.countDocuments({}), maxCount);

    if (expectedIndexes) {
        const actualIndexes = coll.getIndexes();
        jsTest.log("*** Checking expectedIndexes " + tojson({actualIndexes, expectedIndexes}));
        for (let expectedIndex of expectedIndexes) {
            let found = 0;
            for (let actualIndex of actualIndexes) {
                if (expectedIndex.name != actualIndex.name) {
                    continue;
                }
                found = true;
                for (let fieldName in expectedIndex) {
                    const actual = getDottedField(actualIndex, fieldName);
                    const expected = expectedIndex[fieldName];
                    assert.eq(bsonUnorderedFieldsCompare(actual, expected),
                              0,
                              {fieldName, actual, expected});
                }
            }
            assert(found, {expectedIndex});
        }
    }

    // Validate config.collections doc.
    const ns = listCollectionsDoc.type == "timeseries" ? (dbName + ".system.buckets." + collName)
                                                       : (dbName + "." + collName);
    const collDoc = conn.getCollection("config.collections").findOne({_id: ns});
    if (expectNoShardingMetadata) {
        assert.eq(collDoc, null);
    } else {
        assert.eq(collDoc._id, ns, collDoc);
        if (shardKey) {
            assert(!collDoc.hasOwnProperty("unsplittable"), collDoc);
            assert.eq(collDoc.key, shardKey, collDoc);
        } else {
            assert.eq(collDoc.unsplittable, true, collDoc);
        }
    }
}

const maxCount = 100;
const maxBytes = 100 * Object.bsonsize(makeDocument(0));
const shardKey0 = {
    x: 1
};
const shardKey1 = {
    y: 1
};

const testCases = [
    {
        name: "basic",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey);
        }
    },
    {
        name: "cappedBytes",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand({
                create: collName,
                capped: true,
                size: maxBytes,
            }));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName, {
                // Doing batched inserts in a capped collection is not allowed.
                useBatch: false
            });
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "capped": true,
                    "size": maxBytes,
                }
            });
        },
        // Cannot shard capped collections.
        expectedShardCollectionError: ErrorCodes.InvalidOptions,
    },
    {
        name: "cappedCount",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand(
                {create: collName, capped: true, max: maxCount, size: maxBytes}));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName, {
                // Doing batched inserts in a capped collection is not allowed.
                useBatch: false
            });
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "capped": true,
                    "size": maxBytes,
                    "max": maxCount,
                }
            });
        },
        // Cannot shard capped collections.
        expectedShardCollectionError: ErrorCodes.InvalidOptions,
    },
    {
        name: "idIndex",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand({
                create: collName,
                idIndex: {key: {_id: 1}, name: "_id_", v: 2},
            }));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedIndexes: [
                    {key: {_id: 1}, name: "_id_", v: 2},
                ]
            });
        },
    },
    {
        name: "storageEngine",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand({
                create: collName,
                storageEngine: {
                    wiredTiger: {configString: "allocation_size=4KB,internal_page_max=4KB"},
                },
            }));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "storageEngine.wiredTiger.configString":
                        "allocation_size=4KB,internal_page_max=4KB",
                }
            });
        },
    },
    {
        name: "validator",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand({
                create: collName,
                validator: {z: {$gte: 0}},
            }));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "validator": {z: {$gte: 0}},
                }
            });
            const coll = conn.getDB(dbName).getCollection(collName);
            const doc = coll.findOne({z: 1});
            assert.commandFailedWithCode(coll.update(doc, {$set: {z: -1}}),
                                         ErrorCodes.DocumentValidationFailure);
        },
    },
    {
        name: "validatorLevel",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand(
                {create: collName, validator: {z: {$gte: 0}}, validationLevel: "strict"}));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "validator": {z: {$gte: 0}},
                    "validationLevel": "strict",
                }
            });
            const coll = conn.getDB(dbName).getCollection(collName);
            const doc = coll.findOne({z: 1});
            assert.commandFailedWithCode(coll.update(doc, {$set: {z: -1}}),
                                         ErrorCodes.DocumentValidationFailure);
        },
    },
    {
        name: "indexOptionDefaults",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand({
                create: collName,
                indexOptionDefaults: {
                    storageEngine:
                        {wiredTiger: {configString: "allocation_size=4KB,internal_page_max=4KB"}}
                }
            }));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "indexOptionDefaults.storageEngine.wiredTiger.configString":
                        "allocation_size=4KB,internal_page_max=4KB",
                }
            });
        },
    },
    {
        name: "viewOn",
        createCollection: (conn, dbName, collName) => {
            const viewName = collName + "ViewEmptyPipeline";
            assert.commandWorked(conn.getDB(dbName).runCommand({
                create: viewName,
                viewOn: collName,
                pipeline: [],
            }));
            return dbName + "." + viewName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                // TODO (SERVER-86443): Enable featureFlagTrackUnshardedCollectionsUponCreation.
                expectNoShardingMetadata:
                    !FeatureFlagUtil.isEnabled(conn, "TrackUnshardedCollectionsUponCreation")
            });
        },
        // Cannot move or shard a view.
        expectedMoveCollectionError: ErrorCodes.NamespaceNotFound,
        expectedShardCollectionError: ErrorCodes.CommandNotSupportedOnView,
    },
    {
        name: "pipeline",
        createCollection: (conn, dbName, collName) => {
            const viewName = collName + "ViewNonEmptyPipeline";
            assert.commandWorked(conn.getDB(dbName).runCommand({
                create: viewName,
                viewOn: collName,
                pipeline: [{$match: {z: {$gte: 10}}}],
            }));
            return dbName + "." + viewName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                // TODO (SERVER-86443): Enable featureFlagTrackUnshardedCollectionsUponCreation.
                expectNoShardingMetadata:
                    !FeatureFlagUtil.isEnabled(conn, "TrackUnshardedCollectionsUponCreation")
            });
        },
        // Cannot move or shard a view.
        expectedShardCollectionError: ErrorCodes.CommandNotSupportedOnView,
        expectedMoveCollectionError: ErrorCodes.NamespaceNotFound,
    },
    {
        name: "simpleCollation",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({create: collName, collation: {locale: "simple"}}));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {});
        },
    },
    {
        name: "nonSimpleCollation",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand(
                {create: collName, collation: {locale: "en_US", strength: 1, caseLevel: false}}));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "collation.locale": "en_US",
                    "collation.strength": 1,
                    "collation.caseLevel": false,
                },
                expectedIndexes: [{
                    v: 2,
                    key: {_id: 1},
                    name: "_id_",
                    "collation.locale": "en_US",
                    "collation.strength": 1,
                    "collation.caseLevel": false,
                }]
            });
        },
        // Cannot use a shard key whose supporting index has non-simple collation.
        expectedShardCollectionError: ErrorCodes.BadValue,
    },
    {
        name: "changeStreamPreAndPostImages",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand(
                {create: collName, changeStreamPreAndPostImages: {enabled: true}}));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "changeStreamPreAndPostImages.enabled": true,
                }
            });
        },
    },
    {
        name: "timeseries",
        shouldSkip: (conn) => !FeatureFlagUtil.isEnabled(conn, "ReshardingForTimeseries"),
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand(
                {create: collName, timeseries: {timeField: "x", metaField: "y"}}));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(
                conn, dbName, collName, (shardKey && shardKey.y) ? {meta: 1} : shardKey, {
                    expectedCollOpts: {
                        "timeseries.timeField": "x",
                        "timeseries.metaField": "y",
                        "timeseries.granularity": "seconds",
                        "timeseries.bucketMaxSpanSeconds": 3600,
                    }
                });
        },
    },
    {
        name: "clusteredIndex",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).runCommand({
                create: collName,
                clusteredIndex: {key: {_id: 1}, unique: true, name: "clustered_index", v: 2}
            }));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "clusteredIndex": {key: {_id: 1}, unique: true, name: "clustered_index", v: 2},
                },
                expectedIndexes: [{
                    v: 2,
                    key: {_id: 1},
                    name: "clustered_index",
                    unique: true,
                    clustered: true,
                }],
            });
        },
    },
    {
        name: "recordIdsReplicated",
        // TODO (SERVER-68173): Enable featureFlagRecordIdsReplicated.
        shouldSkip: (conn) => !FeatureFlagUtil.isEnabled(conn, "RecordIdsReplicated"),
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({create: collName, recordIdsReplicated: true}));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "recordIdsReplicated": true,
                }
            });
        },
    },
    {
        name: "expireAfterSeconds",
        createCollection: (conn, dbName, collName) => {
            assert.commandWorked(conn.getDB(dbName).getCollection(collName).createIndex(
                {z: 1}, {name: "z_1", expireAfterSeconds: 5 * 3600}));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            insertDocuments(conn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedIndexes: [{
                    v: 2,
                    key: {z: 1},
                    name: "z_1",
                    expireAfterSeconds: 5 * 3600,
                }]
            });
        },
    },
    {
        name: "encryptedFields",
        shouldSkip: (conn) => !buildInfo().modules.includes("enterprise"),
        createCollection: (conn, dbName, collName) => {
            const encryptedClient = new EncryptedClient(conn, dbName);
            assert.commandWorked(encryptedClient.createEncryptionCollection(collName, {
                encryptedFields: {
                    "fields": [
                        {
                            "path": "z",
                            "bsonType": "int",
                            "queries": {"queryType": "equality"}  // allow single object or array
                        },
                    ]
                }
            }));
            return dbName + "." + collName;
        },
        insertDocuments: (conn, dbName, collName) => {
            const encryptedClient = new EncryptedClient(conn, dbName);
            const encryptedConn = encryptedClient.getDB().getMongo();
            insertDocuments(encryptedConn, dbName, collName);
        },
        validateCollection: (conn, dbName, collName, shardKey) => {
            validateCollection(conn, dbName, collName, shardKey, {
                expectedCollOpts: {
                    "encryptedFields.escCollection": "enxcol_." + collName + ".esc",
                    "encryptedFields.ecocCollection": "enxcol_." + collName + ".ecoc",
                    "encryptedFields.fields.0.path": "z",
                    "encryptedFields.fields.0.bsonType": "int",
                    "encryptedFields.fields.0.queries.queryType": "equality",
                }
            });
        },
    },
];

/*
 * For each test case above, do the following:
 * 1. Create the test collection and insert documents into it.
 * 2. Run the moveCollection command against the collection and validate its sharding metadata and
 *    collection options.
 * 3. Run the shardCollection command and then reshardCollection command against the collection and
 *    validate its sharding metadata and collection options.
 */
function runTest(configShard) {
    const st = new ShardingTest({shards: 2, configShard});
    testCases.forEach(testCase => {
        if (testCase.shouldSkip && testCase.shouldSkip(st.s)) {
            return;
        }

        jsTest.log("Running test " + tojsononeline({testCase: testCase.name, configShard}));
        const dbName = "testDb-" + testCase.name;
        const collName = "testColl";
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
        const ns = testCase.createCollection(st.s, dbName, collName);
        testCase.insertDocuments(st.s, dbName, collName);

        jsTest.log("Test moveCollection " + tojsononeline({testCase: testCase.name, configShard}));
        const moveRes = st.s.adminCommand({moveCollection: ns, toShard: st.shard1.shardName});
        if (testCase.expectedMoveCollectionError) {
            assert.commandFailedWithCode(moveRes, testCase.expectedMoveCollectionError);
        } else {
            assert.commandWorked(moveRes);
        }
        testCase.validateCollection(st.s, dbName, collName, null /* shardKey */);

        jsTest.log("Test reshardCollection " +
                   tojsononeline({testCase: testCase.name, configShard}));
        const coll = st.s.getDB(dbName).getCollection(collName);
        assert.commandWorked(coll.createIndex(shardKey0));
        const shardRes = st.s.adminCommand({shardCollection: ns, key: shardKey0});
        if (testCase.expectedShardCollectionError) {
            assert.commandFailedWithCode(shardRes, testCase.expectedShardCollectionError);
        } else {
            assert.commandWorked(shardRes);
            const reshardRes =
                st.s.adminCommand({reshardCollection: ns, key: shardKey1, numInitialChunks: 1});
            assert.commandWorked(reshardRes);
        }
        testCase.validateCollection(
            st.s, dbName, collName, testCase.expectedShardCollectionError ? null : shardKey1);
    });
    st.stop();
}

runTest(false /* configShard */);
runTest(true /* configShard */);
