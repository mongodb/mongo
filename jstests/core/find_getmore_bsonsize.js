// Ensure that the find and getMore commands can handle documents nearing the 16 MB size limit for
// user-stored BSON documents.
(function() {
    'use strict';

    var cmdRes;
    var collName = 'find_getmore_bsonsize';
    var coll = db[collName];

    coll.drop();

    var oneKB = 1024;
    var oneMB = 1024 * oneKB;

    // Build a (1 MB - 1 KB) string.
    var smallStr = 'x';
    while (smallStr.length < oneMB) {
        smallStr += smallStr;
    }
    assert.eq(smallStr.length, oneMB);
    smallStr = smallStr.substring(0, oneMB - oneKB);

    // Build a (16 MB - 1 KB) string.
    var bigStr = 'y';
    while (bigStr.length < (16 * oneMB)) {
        bigStr += bigStr;
    }
    assert.eq(bigStr.length, 16 * oneMB);
    bigStr = bigStr.substring(0, (16 * oneMB) - oneKB);

    // Collection has one ~1 MB doc followed by one ~16 MB doc.
    assert.writeOK(coll.insert({_id: 0, padding: smallStr}));
    assert.writeOK(coll.insert({_id: 1, padding: bigStr}));

    // Find command should just return the first doc, as adding the last would create an invalid
    // command response document.
    cmdRes = db.runCommand({find: collName});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 1);

    // The 16 MB doc should be returned alone on getMore. This is the last document in the
    // collection, so the server should close the cursor.
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, 1);

    // Setup a cursor without returning any results (batchSize of zero).
    cmdRes = db.runCommand({find: collName, batchSize: 0});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 0);

    // First getMore should only return one doc, since both don't fit in the response.
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, 1);

    // Second getMore should return the second doc and a third will close the cursor.
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, 1);

    coll.drop();

    // Insert a document of exactly 16MB and make sure the find command can return it.
    bigStr = 'y';
    while (bigStr.length < (16 * oneMB)) {
        bigStr += bigStr;
    }
    bigStr = bigStr.substring(0, (16 * oneMB) - 32);
    var maxSizeDoc = {_id: 0, padding: bigStr};
    assert.eq(Object.bsonsize(maxSizeDoc), 16 * oneMB);
    assert.writeOK(coll.insert(maxSizeDoc));

    cmdRes = db.runCommand({find: collName});
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 1);
})();
