// SERVER-24963/SERVER-27930 Missing invalidation for system.indexes writes
(function() {
    'use strict';
    var collName = 'system_indexes_invalidations';
    var collNameRenamed = 'renamed_collection';
    var coll = db[collName];
    var collRenamed = db[collNameRenamed];

    function testIndexInvalidation(isRename) {
        coll.drop();
        collRenamed.drop();
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
        assert.commandWorked(coll.createIndex({c: 1}));

        // Get the first two indexes. Use find on 'system.indexes' on MMAPv1, listIndexes otherwise.
        var cursor;
        if (db.system.indexes.count()) {
            cursor = db.system.indexes.find().batchSize(2);
        } else {
            var res = db.runCommand({listIndexes: collName});
            assert.commandWorked(res, 'could not run listIndexes');
            cursor = new DBCommandCursor(db.getMongo(), res);
        }

        // Ensure the cursor has data, rename or drop the collection, and exhaust the cursor.
        var errMsg = 'expected more data from system.indexes find cursor';
        assert(cursor.hasNext(), errMsg);
        if (isRename) {
            assert.commandWorked(coll.renameCollection(collNameRenamed));
        } else {
            assert(coll.drop());
        }
        assert.gt(cursor.itcount(), 0, errMsg);
    }

    // Test that we invalidate indexes for both collection drops and renames.
    testIndexInvalidation(false);
    testIndexInvalidation(true);
}());
