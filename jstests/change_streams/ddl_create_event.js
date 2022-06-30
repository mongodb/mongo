/**
 * Test that change streams returns create events which captures the options specified on the
 * original user command.
 *
 * @tags: [ requires_fcv_60, ]
 */
(function() {
"use strict";

load('jstests/libs/collection_drop_recreate.js');  // For 'assertDropAndRecreateCollection' and
                                                   // 'assertDropCollection'.
load('jstests/libs/change_stream_util.js');        // For 'ChangeStreamTest' and
                                                   // 'assertChangeStreamEventEq'.

const testDB = db.getSiblingDB(jsTestName());

const dbName = testDB.getName();
const collName = jsTestName();
const ns = {
    db: dbName,
    coll: collName
};

const test = new ChangeStreamTest(testDB);

function getCollectionUuid(coll) {
    const collInfo = testDB.getCollectionInfos({name: coll})[0];
    return collInfo.info.uuid;
}

function assertNextChangeEvent(cursor, expectedEvent) {
    const event = test.getOneChange(cursor);

    // Check the presence and the type of 'wallTime' field. We have no way to check the correctness
    // of 'wallTime' value, so we delete it afterwards.
    assert(event.wallTime instanceof Date);
    delete event.wallTime;

    expectedEvent.collectionUUID = getCollectionUuid(collName);
    assertChangeStreamEventEq(event, expectedEvent);
}

function runTest(startChangeStream) {
    function validateExpectedEventAndDropCollection(command, expectedOutput) {
        const cursor = startChangeStream();
        assert.commandWorked(testDB.runCommand(command));
        assertNextChangeEvent(cursor, expectedOutput);
        assertDropCollection(testDB, collName);
    }

    // Basic create collection command.
    validateExpectedEventAndDropCollection({create: collName}, {
        operationType: "create",
        ns: ns,
        operationDescription: {idIndex: {v: 2, key: {_id: 1}, name: "_id_"}}
    });

    // With implicit create collection through insert.
    let cursor = startChangeStream();
    assert.commandWorked(testDB.runCommand({insert: collName, documents: [{_id: 0}]}));
    assertNextChangeEvent(cursor, {
        operationType: "create",
        ns: ns,
        operationDescription: {idIndex: {v: 2, key: {_id: 1}, name: "_id_"}}
    });
    assertNextChangeEvent(
        cursor, {operationType: "insert", fullDocument: {_id: 0}, ns: ns, documentKey: {_id: 0}});
    assertDropCollection(testDB, collName);

    // With capped collection parameters.
    validateExpectedEventAndDropCollection(
        {create: collName, capped: true, size: 1000, max: 1000}, {
            operationType: "create",
            ns: ns,
            operationDescription:
                {idIndex: {v: 2, key: {_id: 1}, name: "_id_"}, capped: true, size: 1024, max: 1000}
        });

    // With wired tiger setting.
    const customWiredTigerSettings = {wiredTiger: {configString: "block_compressor=zlib"}};
    validateExpectedEventAndDropCollection(
        {
            create: collName,
            indexOptionDefaults: {storageEngine: customWiredTigerSettings},
            storageEngine: customWiredTigerSettings
        },
        {
            operationType: "create",
            ns: ns,
            operationDescription: {
                idIndex: {v: 2, key: {_id: 1}, name: "_id_"},
                indexOptionDefaults: {storageEngine: customWiredTigerSettings},
                storageEngine: customWiredTigerSettings
            }
        });

    // With validator collection parameters.
    validateExpectedEventAndDropCollection(
        {create: collName, validator: {a: 1}, validationLevel: "off", validationAction: "warn"}, {
            operationType: "create",
            ns: ns,
            operationDescription: {
                idIndex: {v: 2, key: {_id: 1}, name: "_id_"},
                validator: {a: 1},
                validationLevel: "off",
                validationAction: "warn"
            }
        });

    // With collation.
    const collation = {
        locale: "en",
        "caseLevel": false,
        "caseFirst": "off",
        "strength": 3,
        "numericOrdering": false,
        "alternate": "non-ignorable",
        "maxVariable": "punct",
        "normalization": false,
        "backwards": true,
        "version": "57.1"
    };
    validateExpectedEventAndDropCollection({create: collName, collation: collation}, {
        operationType: "create",
        ns: ns,
        operationDescription: {
            collation: collation,
            idIndex: {v: 2, key: {_id: 1}, name: "_id_", collation: collation}
        }
    });

    // With clustered index.
    validateExpectedEventAndDropCollection(
        {
            create: collName,
            clusteredIndex: {key: {_id: 1}, name: "newName", unique: true},
            expireAfterSeconds: 10
        },
        {
            operationType: "create",
            ns: ns,
            operationDescription: {
                clusteredIndex: {v: 2, key: {_id: 1}, name: "newName", unique: true},
                expireAfterSeconds: NumberLong(10)
            }
        });

    // With idIndex field.
    validateExpectedEventAndDropCollection(
        {create: collName, idIndex: {v: 2, key: {_id: 1}, name: "ignored"}}, {
            operationType: "create",
            ns: ns,
            operationDescription: {
                idIndex: {v: 2, key: {_id: 1}, name: "_id_"},
            }
        });
    validateExpectedEventAndDropCollection(
        {create: collName, idIndex: {v: 1, key: {_id: 1}, name: "new"}},
        {operationType: "create", ns: ns, operationDescription: {}});

    // Verify that the time-series create command does not produce an event.
    cursor = startChangeStream();
    assert.commandWorked(testDB.runCommand({create: collName, timeseries: {timeField: "t"}}));
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: []});
    assertDropCollection(testDB, collName);

    if (FixtureHelpers.isMongos(db)) {
        cursor = startChangeStream();

        assert.commandWorked(testDB.adminCommand({enableSharding: ns.db}));
        assert.commandWorked(
            testDB.adminCommand({shardCollection: ns.db + "." + collName, key: {a: 1}}));

        assertNextChangeEvent(cursor, {
            operationType: "create",
            ns,
            operationDescription: {
                idIndex: {v: 2, key: {_id: 1}, name: "_id_"},
            }
        });
        assertDropCollection(testDB, collName);
    }
}

const pipeline = [{$changeStream: {showExpandedEvents: true}}];
runTest(() => test.startWatchingChanges({pipeline, collection: 1}));
runTest(() => test.startWatchingChanges({pipeline, collection: collName}));
}());
