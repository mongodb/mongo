// SERVER-24963 Missing invalidation for system.indexes write
(function() {
    'use strict';
    let collName = 'system_indexes_invalidations';
    let coll = db[collName];
    coll.drop();
    assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {c: 1}]));

    // Get the first two indexes. Use find on 'system.indexes' on MMAPv1, listIndexes otherwise.
    let cmd = db.system.indexes.count() ? {find: 'system.indexes'} : {listIndexes: collName};
    Object.extend(cmd, {batchSize: 2});
    let res = db.runCommand(cmd);
    assert.commandWorked(res, 'could not run ' + tojson(cmd));
    printjson(res);

    // Ensure the cursor has data, drop the collection, and exhaust the cursor.
    let cursor = new DBCommandCursor(db.getMongo(), res);
    let errMsg = 'expected more data from command ' + tojson(cmd) + ', with result ' + tojson(res);
    assert(cursor.hasNext(), errMsg);
    assert(coll.drop());
    assert.gt(cursor.itcount(), 0, errMsg);
}());
