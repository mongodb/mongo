// don't allow regex as _id: SERVER-9502

var replTest = new ReplSetTest( {name: "server9502", nodes: 2} );
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();
var mdb = master.getDB("test");
mdb.setWriteConcern({ w: 2, wtimeout: 60000 });
assert.writeOK(mdb.foo.insert({ _id: "ABCDEF" }));

assert.writeError(mdb.foo.insert({ _id: /^A/ }));

// _id doesn't have to be first; still disallowed
assert.writeError(mdb.foo.insert({ xxx: "ABCDEF", _id: /ABCDEF/ }));

