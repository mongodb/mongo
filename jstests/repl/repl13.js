// Test update modifier uassert during initial sync. SERVER-4781
var debuggingEnabled = false;

function debug(x) {
    if (debuggingEnabled) {
        printjson(x);
    }
}

rt = new ReplTest("repl13tests");

m = rt.start(true);
mc = m.getDB('d')['c'];

// Insert some documents with a:{} fields.
var bulk = mc.initializeUnorderedBulkOp();
for (var i = 0; i < 100000; ++i) {
    bulk.insert({_id: i, a: {}});
}
assert.writeOK(bulk.execute());

s = rt.start(false);
sc = s.getDB('d')['c'];

// Wait for the initial clone to begin.
assert.soon(function() {
    debug(sc.count());
    return sc.count() > 0;
});

// Update documents that will be cloned last with the intent that an updated version will be cloned.
// This may cause an assertion when an update that was successfully applied to the original version
// of a document is replayed against an updated version of the same document.
bulk = mc.initializeUnorderedBulkOp();
for (i = 99999; i >= 90000; --i) {
    // If the document is cloned as {a:1}, the {$set:{'a.b':1}} modifier will uassert.
    bulk.find({_id: i}).update({$set: {'a.b': 1}});
    bulk.find({_id: i}).update({$set: {a: 1}});
}
assert.writeOK(bulk.execute());

// The initial sync completes and subsequent writes succeed, in spite of any assertions that occur
// when the update operations above are replicated.
mc.save({});
assert.eq(100001, mc.count());
assert.soon(function() {
    return sc.count() == 100001;
});
mc.save({});
assert.eq(100002, mc.count());
assert.soon(function() {
    return sc.count() == 100002;
});

debug(sc.findOne({_id: 99999}));
debug(sc.findOne({_id: 90000}));

assert.eq(1, sc.findOne({_id: 99999}).a);
assert.eq(1, sc.findOne({_id: 90000}).a);

m_hash = m.getDB("d").runCommand("dbhash");
s_hash = s.getDB("d").runCommand("dbhash");

assert.eq(
    m_hash.collections.c, s_hash.collections.c, "sad " + tojson(m_hash) + " " + tojson(s_hash));
