// SERVER-27996/SERVER-28022 Missing invalidation for system.namespaces writes
(function() {
    'use strict';
    var dbInvalidName = 'system_namespaces_invalidations';
    var dbInvalid = db.getSiblingDB(dbInvalidName);
    var num_collections = 3;
    var DROP = 1;
    var RENAME = 2;
    function testNamespaceInvalidation(namespaceAction, batchSize) {
        dbInvalid.dropDatabase();

        // Create enough collections to necessitate multiple cursor batches.
        for (var i = 0; i < num_collections; i++) {
            assert.commandWorked(dbInvalid.createCollection('coll' + i.toString()));
        }

        // Get the first batch of namespaces. Use find on 'system.namespaces' on MMAPv1,
        // listCollections otherwise.
        var cursor;
        if (dbInvalid.system.namespaces.count()) {
            cursor = dbInvalid.system.namespaces.find().batchSize(batchSize);
        } else {
            var res = dbInvalid.runCommand({listCollections: dbInvalidName});
            assert.commandWorked(res, 'could not run listCollections');
            cursor = new DBCommandCursor(dbInvalid.getMongo(), res);
        }

        // Ensure the cursor has data, invalidate the namespace, and exhaust the cursor.
        var errMsg = 'expected more data from system.namespaces cursor';
        assert(cursor.hasNext(), errMsg);
        if (namespaceAction == RENAME) {
            // Rename the collection to something that does not fit in the previously allocated
            // memory for the record.
            assert.commandWorked(
                dbInvalid['coll1'].renameCollection('coll1' +
                                                    'lkdsahflaksjdhfsdkljhfskladhfkahfsakfla' +
                                                    'skfjhaslfaslfkhasklfjhsakljhdsjksahkldjslh'));
        } else if (namespaceAction == DROP) {
            assert(dbInvalid['coll1'].drop());
        }
        assert.gt(cursor.itcount(), 0, errMsg);
    }
    // Test that we invalidate the old namespace record ID when we remove or rename a namespace
    // record.
    for (var j = 2; j < 7; j++) {
        testNamespaceInvalidation(DROP, j);
        testNamespaceInvalidation(RENAME, j);
    }
}());
