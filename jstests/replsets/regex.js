// don't allow regex as _id: SERVER-9502

var replTest = new ReplSetTest( {name: "server9502", nodes: 2} );
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();
var mdb = master.getDB("test");
mdb.foo.insert({ _id: "ABCDEF" });
var gle = master.getDB("test").runCommand({getLastError : 1, w : 2, wtimeout : 60000});
assert(gle.err === null);

mdb.foo.insert({ _id: /^A/ });
var gle = master.getDB("test").runCommand({getLastError : 1, w : 2, wtimeout : 60000});
assert(gle.code === 16814);

// _id doesn't have to be first; still disallowed
mdb.foo.insert({ xxx: "ABCDEF", _id: /ABCDEF/ });
var gle = master.getDB("test").runCommand({getLastError : 1, w : 2, wtimeout : 60000});
assert(gle.code === 16814);
