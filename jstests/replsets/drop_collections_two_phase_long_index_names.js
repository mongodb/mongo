/**
 * Test to ensure that indexes with long names are handled gracefully during a two phase collection
 * drop rename. If a rename would violate length constraints on MMAPv1, the offending indexes should
 * be physically dropped immediately.
 */

(function() {
    'use strict';

    load("jstests/replsets/libs/two_phase_drops.js");  // For TwoPhaseDropCollectionTest.

    // Return a list of all indexes for a given collection. Use 'args' as the
    // 'listIndexes' command arguments.
    function listIndexes(database, coll, args) {
        args = args || {};
        let failMsg = "'listIndexes' command failed";
        let listIndexesCmd = {listIndexes: coll};
        let res = assert.commandWorked(database.runCommand(listIndexesCmd, args), failMsg);
        return res.cursor.firstBatch;
    }

    // Set up a two phase drop test.
    let testName = "drop_collection_two_phase_long_index_names";
    let dbName = testName;
    let collName = "collToDrop";
    let twoPhaseDropTest = new TwoPhaseDropCollectionTest(testName, dbName);

    // Initialize replica set.
    let replTest = twoPhaseDropTest.initReplSet();

    // Create the collection that will be dropped.
    twoPhaseDropTest.createCollection(collName);

    // Two phase collection drops should handle long index names gracefully. MMAPv1 imposes a hard
    // limit on index namespaces so we have to drop indexes that are too long to store on disk after
    // renaming the collection (see SERVER-29747). Other storage engines should allow the implicit
    // index renames to proceed because these renamed indexes are internal and will not be visible
    // to users (no risk of being exported to another storage engine).
    let coll = replTest.getPrimary().getDB(dbName).getCollection(collName);
    let maxNsLength = 127;
    let longIndexName = ''.pad(maxNsLength - (coll.getFullName() + '.$').length, true, 'a');
    let shortIndexName = "short_name";

    // Create one index with a "too long" name, and one with a name of acceptable size.
    assert.commandWorked(coll.ensureIndex({a: 1}, {name: longIndexName}));
    assert.commandWorked(coll.ensureIndex({b: 1}, {name: shortIndexName}));

    let droppedCollName = twoPhaseDropTest.prepareDropCollection(collName);

    // Check that, on MMAPv1, indexes that would violate the namespace length constraints after
    // rename were dropped.
    let storageEngine = jsTest.options().storageEngine;
    if (storageEngine === 'mmapv1') {
        let primaryDB = replTest.getPrimary().getDB(dbName);
        let indexes = listIndexes(primaryDB, droppedCollName);
        assert(indexes.find(idx => idx.name === shortIndexName));
        assert.eq(undefined, indexes.find(idx => idx.name === longIndexName));
    }

    twoPhaseDropTest.commitDropCollection(collName);

    replTest.stopSet();

})();