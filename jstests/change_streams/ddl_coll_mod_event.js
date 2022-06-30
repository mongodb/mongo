/**
 * Tests the behavior of the 'modify' event via various 'collMod' commands.
 *
 * @tags: [
 *   requires_fcv_60,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/collection_drop_recreate.js');  // For 'assertDropAndRecreateCollection' and
                                                   // 'assertDropCollection'.
load('jstests/libs/change_stream_util.js');        // For 'ChangeStreamTest' and
                                                   // 'assertChangeStreamEventEq'.
load("jstests/libs/fixture_helpers.js");

const testDB = db.getSiblingDB(jsTestName());

const dbName = testDB.getName();
const collName = jsTestName();
const ns = {
    db: dbName,
    coll: collName
};

const pipeline = [{$changeStream: {showExpandedEvents: true}}];
const cst = new ChangeStreamTest(testDB);

function getCollectionUuid(coll) {
    const collInfo = testDB.getCollectionInfos({name: coll})[0];
    return collInfo.info.uuid;
}

function assertNextChangeEvent(cursor, expectedEvent) {
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expectedEvent});
}

function runTest(startChangeStream) {
    assert.commandWorked(testDB.runCommand({create: collName}));
    assert.commandWorked(testDB[collName].insert({a: 1, b: 1, c: 1}));

    function testCollModValidator() {
        let cursor = startChangeStream();
        let options = {
            schemaValidator: {
                '$jsonSchema': {
                    'bsonType': "object",
                    'properties': {
                        'a': {
                            'bsonType': "number",
                        },
                        'b': {
                            'bsonType': "number",
                        },
                        'c': {
                            'bsonType': "number",
                        },
                    }
                }
            },
            validationLevel: "strict",
            validationAction: "error"
        };

        assert.commandWorked(
            testDB[collName].runCommand({collMod: collName, validator: options.schemaValidator}));

        const numShards = FixtureHelpers.numberOfShardsForCollection(testDB[collName]);

        let expectedChanges = [];
        for (let i = 0; i < numShards; ++i) {
            expectedChanges.push({
                operationType: "modify",
                ns: ns,
                operationDescription: {validator: options.schemaValidator},
                stateBeforeChange: {
                    collectionOptions: {uuid: getCollectionUuid(collName)},
                }
            });
        }
        assertNextChangeEvent(cursor, expectedChanges);

        // Modify the validation level.
        const newValidationLevel = "off";
        assert.commandWorked(
            testDB[collName].runCommand({collMod: collName, validationLevel: newValidationLevel}));

        expectedChanges = [];
        for (let i = 0; i < numShards; ++i) {
            expectedChanges.push({
                operationType: "modify",
                ns: ns,
                operationDescription: {validationLevel: newValidationLevel},
                stateBeforeChange: {
                    collectionOptions: {
                        uuid: getCollectionUuid(collName),
                        validator: options.schemaValidator,
                        validationLevel: options.validationLevel,
                        validationAction: options.validationAction
                    }
                }
            });
        }
        assertNextChangeEvent(cursor, expectedChanges);

        options.validationLevel = newValidationLevel;

        // Modify the validation action, i.e. the parameter which determined whether to error on
        // invalid documents or warn.
        const newValidationAction = "warn";
        assert.commandWorked(testDB[collName].runCommand(
            {collMod: collName, validationAction: newValidationAction}));

        expectedChanges = [];
        for (let i = 0; i < numShards; ++i) {
            expectedChanges.push({
                operationType: "modify",
                ns: ns,
                operationDescription: {validationAction: newValidationAction},
                stateBeforeChange: {
                    collectionOptions: {
                        uuid: getCollectionUuid(collName),
                        validator: options.schemaValidator,
                        validationLevel: options.validationLevel,
                        validationAction: options.validationAction
                    }
                }
            });
        }
        assertNextChangeEvent(cursor, expectedChanges);
    }

    function testCollModIndex(key, options) {
        // Determine what "indexName" should be based on "key".
        let arr = [];
        for (let property in key) {
            arr.push(property);
            arr.push(key[property]);
        }
        let indexName = arr.join("_");
        let opDesc = {indexes: [Object.assign({v: 2, key: key, name: indexName}, options)]};

        // Create an index.
        assert.commandWorked(testDB[collName].createIndex(key, options));

        // Start watching for change stream events.
        let cursor = startChangeStream();

        // Modify the collection by issuing a command to modify the index, toggling the 'hidden'
        // option on the index. We toggle twice in order to bring back the collection index options
        // into the state it started with.
        const toggleIndexHiddenOp = {
            index: {
                name: indexName,
                hidden: options.hidden ? false : true,
            }
        };
        const undoToggleIndexHiddenOp = {
            index: {
                name: indexName,
                hidden: options.hidden ? true : false,
            }
        };
        assert.commandWorked(
            testDB[collName].runCommand({collMod: collName, index: toggleIndexHiddenOp.index}));
        assert.commandWorked(
            testDB[collName].runCommand({collMod: collName, index: undoToggleIndexHiddenOp.index}));

        const numShards = FixtureHelpers.numberOfShardsForCollection(testDB[collName]);

        let expectedChanges = [];
        for (let i = 0; i < numShards; ++i) {
            expectedChanges.push({
                operationType: "modify",
                ns: ns,
                operationDescription: toggleIndexHiddenOp,
                stateBeforeChange: {
                    collectionOptions: {uuid: getCollectionUuid(collName)},
                    indexOptions: {hidden: !toggleIndexHiddenOp.index.hidden}
                }
            });
        }
        // First event toggling the 'hidden' option on the index.
        assertNextChangeEvent(cursor, expectedChanges);

        expectedChanges = [];
        for (let i = 0; i < numShards; ++i) {
            expectedChanges.push({
                operationType: "modify",
                ns: ns,
                operationDescription: undoToggleIndexHiddenOp,
                stateBeforeChange: {
                    collectionOptions: {uuid: getCollectionUuid(collName)},
                    indexOptions: {hidden: !undoToggleIndexHiddenOp.index.hidden}
                }
            });
        }
        // Second event restoring the 'hidden' option on the index to the initial state.
        assertNextChangeEvent(cursor, expectedChanges);

        // Modify the collection by issuing a command to modify the index, this time changing the
        // TTL expiration threshold.
        if (options.expireAfterSeconds) {
            const modifyIndexExpireAfterSecondsOp = {
                index: {name: indexName, expireAfterSeconds: NumberLong(100000)}
            };
            assert.commandWorked(testDB[collName].runCommand(
                {collMod: collName, index: modifyIndexExpireAfterSecondsOp.index}));

            expectedChanges = [];
            for (let i = 0; i < numShards; ++i) {
                expectedChanges.push({
                    operationType: "modify",
                    ns: ns,
                    operationDescription: modifyIndexExpireAfterSecondsOp,
                    stateBeforeChange: {
                        collectionOptions: {uuid: getCollectionUuid(collName)},
                        indexOptions: options
                    }
                });
            }
            assertNextChangeEvent(cursor, expectedChanges);
        }

        // Drop the index.
        assert.commandWorked(testDB[collName].dropIndexes([indexName]));
    }

    // Test 'collMod' commands by modifying index options on the collection.
    let options = {};
    testCollModIndex({a: 1}, options);

    options = {hidden: true};
    testCollModIndex({b: 1}, options);

    options = {expireAfterSeconds: NumberLong(100000)};
    testCollModIndex({c: 1}, options);

    // Test 'collMod' commands by modifying validation options on the collection.
    testCollModValidator();

    testDB[collName].drop();
}

// Run the test using a whole-db collection change stream.
runTest((() => cst.startWatchingChanges({pipeline, collection: 1})));

// Run the test using a single collection change stream.
runTest((() => cst.startWatchingChanges({pipeline, collection: collName})));
}());
