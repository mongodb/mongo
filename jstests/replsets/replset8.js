
// test for SERVER-6303 - if documents move backward during an initial sync.

var rt = new ReplSetTest({name: "replset8tests", nodes: 1});

var nodes = rt.startSet();
rt.initiate();
var master = rt.getPrimary();
var bigstring = "a";
var md = master.getDB('d');
var mdc = md['c'];

// prep the data

// idea: create x documents of increasing size, then create x documents of size n.
//       delete first x documents.  start initial sync (cloner).  update all remaining
//       documents to be increasing size.
//       this should result in the updates moving the docs backwards.

var doccount = 5000;
// Avoid empty extent issues
mdc.insert({_id: -1, x: "dummy"});

print("inserting bigstrings");
var bulk = mdc.initializeUnorderedBulkOp();
for (i = 0; i < doccount; ++i) {
    bulk.insert({_id: i, x: bigstring});
    bigstring += "a";
}
assert.writeOK(bulk.execute());

print("inserting x");
bulk = mdc.initializeUnorderedBulkOp();
for (i = doccount; i < doccount * 2; ++i) {
    bulk.insert({_id: i, x: i});
}
assert.writeOK(bulk.execute());

print("deleting bigstrings");
bulk = mdc.initializeUnorderedBulkOp();
for (i = 0; i < doccount; ++i) {
    bulk.find({_id: i}).remove();
}
assert.writeOK(bulk.execute());

// add a secondary
var slave = rt.add();
rt.reInitiate();
print("initiation complete!");
rt.awaitSecondaryNodes();
print("updating documents backwards");
// Move all documents to the beginning by growing them to sizes that should
// fit the holes we made in phase 1
bulk = mdc.initializeUnorderedBulkOp();
for (i = doccount * 2; i > doccount; --i) {
    bulk.find({_id: i, x: i}).update({$set: {x: bigstring}});
    bigstring = bigstring.slice(0, -1);  // remove last char
}
assert.writeOK(bulk.execute({writeConcern: {w: rt.nodes.length}}));
print("finished");
assert.eq(doccount + 1, slave.getDB('d')['c'].find().itcount());
