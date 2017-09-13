/**
 * This test creates a collection with autoIndexId: false. It then copies the database and expects
 * the new collection to not have an _id index either.
 */

(function() {
    "use strict";
    var db1 = db.getSiblingDB('copydatabase_no_id_index');
    var db2 = db.getSiblingDB('copydatabase_no_id_index2');
    db1.dropDatabase();
    db2.dropDatabase();

    assert.commandWorked(db1.runCommand({create: 'foo', autoIndexId: false}));
    assert.writeOK(db1.foo.insert({a: 1}));
    assert.eq(db1.foo.getIndexes().length, 0);

    assert.commandWorked(db1.copyDatabase('copydatabase_no_id_index', 'copydatabase_no_id_index2'));

    assert.eq(db1.foo.count(), 1);
    assert.eq(db1.foo.getIndexes().length, 0);
    assert.eq(db2.foo.count(), 1);
    assert.eq(db2.foo.getIndexes().length, 0);
})();