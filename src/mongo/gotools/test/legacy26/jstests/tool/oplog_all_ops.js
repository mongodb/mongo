/**
 * Performs a simple test on mongooplog by doing different types of operations
 * that will show up in the oplog then replaying it on another replica set.
 * Correctness is verified using the dbhash command.
 */

var repl1 = new ReplSetTest({ name: 'rs1', nodes: [{ nopreallocj: '' },
    { arbiter: true }, { arbiter: true }]});

repl1.startSet({ oplogSize: 10 });
repl1.initiate();
repl1.awaitSecondaryNodes();

var repl1Conn = new Mongo(repl1.getURL());
var testDB = repl1Conn.getDB('test');
var testColl = testDB.user;

// op i
testColl.insert({ x: 1 });
testColl.insert({ x: 2 });

// op c
testDB.dropDatabase();

testColl.insert({ y: 1 });
testColl.insert({ y: 2 });
testColl.insert({ y: 3 });

// op u
testColl.update({}, { $inc: { z: 1 }}, true, true);

// op d
testColl.remove({ y: 2 });

// op n
var oplogColl = repl1Conn.getCollection('local.oplog.rs');
oplogColl.insert({ ts: new Timestamp(), op: 'n', ns: testColl.getFullName(), 'o': { x: 'noop' }});

var repl2 = new ReplSetTest({ name: 'rs2', startPort: 31100, nodes: [{ nopreallocj: '' },
    { arbiter: true }, { arbiter: true }]});

repl2.startSet({ oplogSize: 10 });
repl2.initiate();
repl2.awaitSecondaryNodes();

var srcConn = repl1.getPrimary();
runMongoProgram('mongooplog', '--from', repl1.getPrimary().host,
    '--host', repl2.getPrimary().host);

var repl1Hash = testDB.runCommand({ dbhash: 1 });

var repl2Conn = new Mongo(repl2.getURL());
var testDB2 = repl2Conn.getDB(testDB.getName());
var repl2Hash = testDB2.runCommand({ dbhash: 1 });

assert(repl1Hash.md5);
assert.eq(repl1Hash.md5, repl2Hash.md5);

repl1.stopSet();
repl2.stopSet();

