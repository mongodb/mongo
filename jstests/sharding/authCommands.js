// Test that when authenticated as the system user, commands use only the auth credentials supplied
// in the $auth field of the command object.

var port = allocatePorts(1)[0];
var path = "jstests/libs/";
MongoRunner.runMongod({port : port, keyFile : path + "key1"})

db = new Mongo('localhost:' + port).getDB('test');

assert.eq(1, db.runCommand({whatsmyuri : 1}).ok);

db.getSiblingDB('admin').addUser("admin", "password"); // active auth even though we're on localhost

assert.eq(0, db.runCommand({whatsmyuri : 1}).ok);

db.getSiblingDB('local').auth('__system', 'foopdedoop');

assert.eq(0, db.runCommand({whatsmyuri : 1}).ok);
assert.eq(1, db.runCommand({whatsmyuri : 1, $auth : { test : { userName : NumberInt(1) } } } ).ok );
assert.eq(0, db.runCommand({whatsmyuri : 1}).ok); // Make sure the credentials are temporary.
assert.eq(0, db.runCommand({dropDatabase : 1, $auth : { test : { userName : NumberInt(1) } } } ).ok );
assert.eq(1, db.runCommand({dropDatabase : 1, $auth : { test : { userName : NumberInt(2) } } } ).ok );