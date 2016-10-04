// test preservePadding

var mydb = db.getSiblingDB('compactPreservePadding');
var collName = "compactPreservePadding";
var t = mydb.getCollection(collName);
t.drop();

// use larger keyname to avoid hitting an edge case with extents
for (i = 0; i < 10000; i++) {
    t.insert({useLargerKeyName: i});
}

// remove half the entries
t.remove({useLargerKeyName: {$mod: [2, 0]}});
printjson(t.stats());
originalSize = t.stats().size;
originalStorage = t.stats().storageSize;

// compact!
mydb.runCommand({compact: collName, preservePadding: true});
printjson(t.stats());

// object sizes ('size') should be the same (unless we hit an edge case involving extents, which
// this test doesn't) and storage size should shrink
assert(originalSize == t.stats().size);
assert(originalStorage > t.stats().storageSize);
