// Test repair on master

var baseName = "jstests_repl_repair";

rt = new ReplTest(baseName);

m = rt.start(true);

m.getDB(baseName)[baseName].save({});
var c = m.getDB('local').oplog.$main.count();
assert.automsg("c > 0");

assert.commandWorked(m.getDB("local").repairDatabase());
assert.automsg("c <= m.getDB( 'local' ).oplog.$main.count()");
