// SERVER-27996 Missing invalidation for system.namespaces writes
(function() {
    'use strict';
    let dbInvalidName = 'system_namespaces_invalidations';
    let dbInvalid = db.getSiblingDB(dbInvalidName);
    let num_collections = 3;
    function testNamespaceInvalidation(isRename) {
        dbInvalid.dropDatabase();

        // Create enough collections to necessitate multiple cursor batches.
        for (let i = 0; i < num_collections; i++) {
            assert.commandWorked(dbInvalid.createCollection('coll' + i.toString()));
        }

        // Get the first two namespaces. Use find on 'system.namespaces' on MMAPv1, listCollections
        // otherwise.
        let cmd = dbInvalid.system.indexes.count() ? {find: 'system.namespaces'}
                                                   : {listCollections: dbInvalidName};
        Object.extend(cmd, {batchSize: 2});
        let res = dbInvalid.runCommand(cmd);
        assert.commandWorked(res, 'could not run ' + tojson(cmd));
        printjson(res);

        // Ensure the cursor has data, drop or rename the collections, and exhaust the cursor.
        let cursor = new DBCommandCursor(dbInvalid.getMongo(), res);
        let errMsg =
            'expected more data from command ' + tojson(cmd) + ', with result ' + tojson(res);
        assert(cursor.hasNext(), errMsg);
        for (let j = 0; j < num_collections; j++) {
            if (isRename) {
                // Rename the collection to something that does not fit in the previously allocated
                // memory for the record.
                assert.commandWorked(dbInvalid['coll' + j.toString()].renameCollection(
                    'coll' + j.toString() + 'lkdsahflaksjdhfsdkljhfskladhfkahfsakfla' +
                    'skfjhaslfaslfkhasklfjhsakljhdsjksahkldjslh'));
            } else {
                assert(dbInvalid['coll' + j.toString()].drop());
            }
        }
        assert.gt(cursor.itcount(), 0, errMsg);
    }
    // Test that we invalidate namespaces for both collection drops and renames.
    testNamespaceInvalidation(false);
    testNamespaceInvalidation(true);
}());
