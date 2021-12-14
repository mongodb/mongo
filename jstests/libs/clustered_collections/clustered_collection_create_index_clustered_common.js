/**
 * Encapsulates testing that verifies the behavior of createIndexes with the 'clustered' option. The
 * 'clustered' option may be used to implicitly create a clustered collection via createIndexes.
 */
const CreateIndexesClusteredTest = (function() {
    "use strict";

    /**
     * From createIndex with the 'clustered' option 'true', generates the corresponding
     * createCollection options for the implicitly created 'coll'.
     */
    const getImplicitCreateOptsFromCreateIndex = function(cmd) {
        const fullCreateOptions =
            ClusteredCollectionUtil.constructFullCreateOptions({clusteredIndex: cmd.indexes[0]});
        delete fullCreateOptions.clusteredIndex.clustered;
        return fullCreateOptions;
    };

    /**
     * Asserts that running the createIndexes 'cmd' implicitly creates a clustered collection.
     */
    const assertCreateIndexesImplicitCreateSucceeds = function(testDB, collName, cmd) {
        assertDropCollection(testDB, collName);
        assert.commandWorked(testDB.runCommand(cmd));

        // From the createIndex command, generate the corresponding createCollection options.
        const fullCreateOptions = getImplicitCreateOptsFromCreateIndex(cmd);

        ClusteredCollectionUtil.validateListCollections(testDB, collName, fullCreateOptions);
        ClusteredCollectionUtil.validateListIndexes(testDB, collName, fullCreateOptions);
    };

    /**
     * Asserts that running createIndexes 'cmd' on a non-existant collection fails with 'errorCode'.
     */
    const assertCreateIndexesImplicitCreateFails = function(testDB, collName, cmd, errorCode) {
        assertDropCollection(testDB, collName);
        assert.commandFailedWithCode(
            testDB.runCommand(cmd),
            errorCode,
            `Expected indexOpts ${tojson(cmd)} to fail with error code ${errorCode}`);
    };

    /**
     * Tests that createIndex with the 'clustered' option fails when a collection exists and is not
     * clustered.
     */
    const runNonClusteredCollectionTest = function(testDB, collName) {
        assertDropCollection(testDB, collName);
        const testColl = testDB[collName];
        assert.commandWorked(testDB.createCollection(collName));

        // Start with the collection empty.
        assert.commandFailedWithCode(
            testColl.createIndex({_id: 1}, {clustered: true, unique: true}), 6100905);
        assert.commandFailedWithCode(testColl.createIndex({a: 1}, {clustered: true, unique: true}),
                                     6100905);

        // Insert some docs. Sometimes empty collections are treated as special when it comes to
        // index builds.
        const batchSize = 100;
        const bulk = testColl.initializeUnorderedBulkOp();
        for (let i = 0; i < batchSize; i++) {
            bulk.insert({_id: i, a: -i});
        }
        assert.commandWorked(bulk.execute());
        assert.commandFailedWithCode(
            testColl.createIndex({_id: 1}, {clustered: true, unique: true}), 6100905);
        assert.commandFailedWithCode(testColl.createIndex({a: 1}, {clustered: true, unique: true}),
                                     6100905);
    };

    /**
     * Tests running createIndex on a clustered collection
     */
    const runClusteredCollectionTest = function(testDB, collName) {
        assertDropCollection(testDB, collName);

        const createOptions = {
            clusteredIndex: {key: {_id: 1}, name: "theClusterKeyName", unique: true}
        };
        assert.commandWorked(testDB.createCollection(collName, createOptions));

        // Confirm we start out with a valid clustered collection.
        const fullCreateOptions = ClusteredCollectionUtil.constructFullCreateOptions(createOptions);
        ClusteredCollectionUtil.validateListCollections(testDB, collName, fullCreateOptions);
        ClusteredCollectionUtil.validateListIndexes(testDB, collName, fullCreateOptions);

        const testColl = testDB[collName];

        // createIndex on the cluster key is a no-op.
        assert.commandWorked(testColl.createIndex({_id: 1}));
        ClusteredCollectionUtil.validateListIndexes(testDB, collName, fullCreateOptions);

        // createIndex on the cluster key with the 'clustered' option is a no-op.
        assert.commandWorked(testColl.createIndex({_id: 1}, {clustered: true, unique: true}));

        // 'clustered' is not a valid option for an index not on the cluster key.
        assert.commandFailedWithCode(
            testColl.createIndex({notMyIndex: 1}, {clustered: true, unique: true}), 6100904);

        // Insert some docs. Sometimes empty collections are treated as special when it comes to
        // index builds.
        const batchSize = 100;
        const bulk = testColl.initializeUnorderedBulkOp();
        for (let i = 0; i < batchSize; i++) {
            bulk.insert({_id: i, a: -i});
        }
        assert.commandWorked(bulk.execute());

        assert.commandWorked(testColl.createIndex({_id: 1}));
        assert.commandWorked(testColl.createIndex({_id: 1}, {clustered: true, unique: true}));

        // Note: this a quirk of how we handle the 'name' field for indexes of {_id: 1}. The
        // createIndex is still a no-op, and the specified name is discarded.
        //
        // Only in implicit collection creation on a non-existent collection can createIndex create
        // a clusteredIndex with a custom name.
        assert.commandWorked(testColl.createIndex({_id: 1}, {name: "notTheClusterKeyName"}));

        ClusteredCollectionUtil.validateListIndexes(testDB, collName, fullCreateOptions);
    };

    /**
     * Runs test cases where createIndexes with 'clustered' should succeed in implicit collecton
     * creation, regardless of whether the database is replicated.
     */
    const runBaseSuccessTests = function(testDB, collName) {
        assertCreateIndexesImplicitCreateSucceeds(testDB, collName, {
            createIndexes: collName,
            indexes: [{key: {_id: 1}, name: "_id_", clustered: true, unique: true}]
        });
        assertCreateIndexesImplicitCreateSucceeds(testDB, collName, {
            createIndexes: collName,
            indexes: [{key: {_id: 1}, name: "_id_", clustered: true, unique: true, v: 2}]
        });
        assertCreateIndexesImplicitCreateSucceeds(testDB, collName, {
            createIndexes: collName,
            indexes: [{key: {_id: 1}, name: "uniqueIdName", clustered: true, unique: true, v: 2}]
        });
    };

    /**
     * Runs test cases where createIndexes with 'clustered' fails, regardless of whether the
     * database is replciated.
     */
    const runBaseFailureTests = function(testDB, collName) {
        // Missing 'unique' option.
        assertCreateIndexesImplicitCreateFails(
            testDB,
            collName,
            {createIndexes: collName, indexes: [{key: {_id: 1}, name: "_id_", clustered: true}]},
            ErrorCodes.CannotCreateIndex);
        // Two 'clustered' indexes.
        assertCreateIndexesImplicitCreateFails(
            testDB,
            collName,
            {
                createIndexes: collName,
                indexes: [
                    {key: {_id: 1}, name: "_id_", clustered: true, unique: true},
                    {key: {a: 1}, name: "a_1", clustered: true, unique: true}
                ]
            },
            6100901);
        assertCreateIndexesImplicitCreateFails(
            testDB,
            collName,
            {
                createIndexes: collName,
                indexes: [
                    {key: {_id: 1}, name: "_id_", clustered: true, unique: true, hidden: true},
                ]
            },
            ErrorCodes.InvalidIndexSpecificationOption);
    };

    /**
     * Runs test cases that are agnostic to whether the database is replicated or not.
     */
    const runBaseTests = function(testDB, collName) {
        runNonClusteredCollectionTest(testDB, collName);
        runClusteredCollectionTest(testDB, collName);
        runBaseSuccessTests(testDB, collName);
        runBaseFailureTests(testDB, collName);
    };

    return {
        assertCreateIndexesImplicitCreateSucceeds: assertCreateIndexesImplicitCreateSucceeds,
        assertCreateIndexesImplicitCreateFails: assertCreateIndexesImplicitCreateFails,
        runBaseTests: runBaseTests,
    };
})();
