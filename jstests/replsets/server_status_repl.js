var rt = new ReplSetTest({name: "server_status_repl", nodes: 2});
rt.startSet();
rt.initiate();

rt.awaitSecondaryNodes();

var secondary = rt.getSecondary();
var primary = rt.getPrimary();
var testDB = primary.getDB("test");

assert.commandWorked(testDB.createCollection('a'));
assert.writeOK(testDB.b.insert({}, {writeConcern: {w: 2}}));

var ss = primary.getDB("test").serverStatus({repl: 1});
assert.neq(ss.repl.replicationProgress, null, tojson(ss.repl));

rt.stopSet();