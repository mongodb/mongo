/*
 * This test ensures that the usePowerOf2 user flag effectively reuses space.
 *
 * As of SERVER-15273 usePowerOf2 is silently ignored so the behavior is the same regardless.
 */

// prepare a doc of 14K
var doc = {_id: new Object(), data: "a"};
var bigDoc = {_id: new Object(), data: "a"};

while (doc.data.length < 14 * 1024)
    doc.data += "a";
while (bigDoc.data.length < 15 * 1024)
    bigDoc.data += "a";

var collName = "usepower1";
var t = db.getCollection(collName);

function checkStorageSize(expectedSize, sameLoc) {
    t.insert(doc);
    assert.eq(t.stats().size, expectedSize, "size should be expected");

    var oldLoc = t.find().showRecordId().toArray()[0].$recordId;

    // Remvoe smaller doc, insert a bigger one.
    t.remove(doc);
    t.insert(bigDoc);

    var newLoc = t.find().showRecordId().toArray()[0].$recordId;

    // Check the diskloc of two docs.
    assert.eq(friendlyEqual(oldLoc, newLoc), sameLoc);
}

t.drop();
db.createCollection(collName);
var res = db.runCommand({"collMod": collName, "usePowerOf2Sizes": false});
assert(res.ok, "collMod failed");
checkStorageSize(16 * 1023, true);  // 15344 = 14369 (bsonsize) + overhead

t.drop();
db.createCollection(collName);
var res = db.runCommand({"collMod": collName, "usePowerOf2Sizes": true});
assert(res.ok, "collMod failed");
checkStorageSize(16 * 1023, true);  // power of 2

// Create collection with flag
t.drop();
db.runCommand({"create": collName, "flags": 0});
checkStorageSize(16 * 1023, true);

t.drop();
db.runCommand({"create": collName, "flags": 1});
checkStorageSize(16 * 1023, true);  // power of 2
