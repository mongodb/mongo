// Alocate collection forcing just a small size remainder in 2nd extent

var baseName = "jstests_disk_newcollection2";
var m = MongoRunner.runMongod({noprealloc: "", smallfiles: ""});
db = m.getDB("test");

db.createCollection(baseName, {size: 0x1FFC0000 - 0x10 - 8192});
var v = db[baseName].validate();
printjson(v);
assert(v.valid);

// Try creating collections with some invalid names and confirm that they
// don't crash MongoD.

db.runCommand({applyOps: [{op: 'u', ns: 'a\0b'}]});
var res = db["a\0a"].insert({});
assert(res.hasWriteError(), "A write to collection a\0a succceeded");
