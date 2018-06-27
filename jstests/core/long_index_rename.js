// SERVER-7720 Building an index with a too-long name should always fail
// Formerly, we would allow an index that already existed to be "created" with too long a name,
// but this caused secondaries to crash when replicating what should be a bad createIndex command.
// Here we test that the too-long name is rejected in this situation as well

(function() {
    'use strict';

    const coll = db.long_index_rename;
    coll.drop();

    for (let i = 1; i < 10; i++) {
        coll.save({a: i});
    }

    // Compute maximum index name length for this collection under FCV 4.0.
    const maxNsLength = 127;
    const maxIndexNameLength = maxNsLength - (coll.getFullName() + ".$").length;
    jsTestLog('Max index name length under FCV 4.0 = ' + maxIndexNameLength);

    // Create an index with the longest name allowed for this collection.
    assert.commandWorked(coll.createIndex({a: 1}, {name: 'a'.repeat(maxIndexNameLength)}));

    // Index namespaces longer than 127 characters are not acceptable.
    assert.commandFailedWithCode(
        coll.createIndex({b: 1}, {name: 'b'.repeat(maxIndexNameLength) + 1}),
        ErrorCodes.CannotCreateIndex);
})();
