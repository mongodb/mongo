// test preservePadding
collName = "compactPreservePadding";
mydb = db.getSisterDB("compactPreservePaddingDB");
mydb.dropDatabase();
mydb.createCollection(collName);
// ensure there is some padding by using power of 2 sizes
mydb.runCommand({collMod: collName, usePowerOf2Sizes: true});
t = mydb.compactPreservePadding;
t.drop();
// populate db
for (i = 0; i < 10000; i++) {
    t.insert({x:i});
}
// remove half the entries
t.remove({x:{$mod:[2,0]}})
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
