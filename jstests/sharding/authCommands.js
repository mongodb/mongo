// Test that when authenticated as the system user, commands use only the auth credentials supplied
// in the $auth field of the command object.

var port = allocatePorts(1)[0];
var path = "jstests/libs/";
MongoRunner.runMongod({port : port, keyFile : path + "key1"})

var db = new Mongo('localhost:' + port).getDB('test');


assert.eq(1, db.runCommand({dbStats : 1}).ok);

db.getSiblingDB('admin').addUser("admin", "password"); // activate auth even though we're on localhost

assert.eq(0, db.runCommand({dbStats : 1}).ok);

assert( db.getSiblingDB('local').auth('__system', 'foopdedoop'), "Failed to authenticate as system user" );

assert.eq(0, db.runCommand({dbStats : 1}).ok);
assert.eq(1, db.runCommand({dbStats : 1, $auth : { test : { userName : NumberInt(1) } } } ).ok );
assert.eq(0, db.runCommand({dbStats : 1}).ok); // Make sure the credentials are temporary.
assert.eq(0, db.runCommand({dropDatabase : 1, $auth : { test : { userName : NumberInt(1) } } } ).ok );
assert.eq(1, db.runCommand({dropDatabase : 1, $auth : { test : { userName : NumberInt(2) } } } ).ok );


db.addUser( "roUser", "password", true ); // Set up read-only user for later

// Test that you can't affect privileges by sending $auth when not authenticated as __system.

db = new Mongo(db.getMongo().host).getDB('test'); // Get new connection with no auth

var runTests = function( db ) {
    assert.eq(0, db.runCommand({dbStats : 1, $auth : { test : { userName : NumberInt(2) } } } ).ok );
    assert.eq(0, db.runCommand({dropDatabase : 1, $auth : { test : { userName : NumberInt(2) } } } ).ok );
    assert.eq(0, db.runCommand({dropDatabase : 1, $auth : { local : { __system : NumberInt(2) } } } ).ok );

    db.auth( "roUser", "password" );

    assert.eq(1, db.runCommand({dbStats : 1}).ok);
    assert.eq(1, db.runCommand({dbStats : 1, $auth : { test : { userName : NumberInt(0) } } } ).ok );
    assert.eq(0, db.runCommand({dropDatabase : 1, $auth : { test : { userName : NumberInt(2) } } } ).ok );
    assert.eq(0, db.runCommand({dropDatabase : 1, $auth : { local : { __system : NumberInt(2) } } } ).ok );
}

runTests( db );

// Test that you can't affect privileges by sending $auth to a sharded system.

var rsOpts = { oplogSize: 10, verbose : 2, useHostname : false };
var st = new ShardingTest({ keyFile : 'jstests/libs/key1', shards : 2, chunksize : 1, config : 3,
                            rs : rsOpts, other : { nopreallocj : 1, verbose : 2, useHostname : false }});

db = st.s.getDB('test');

db.addUser( 'roUser', 'password', true ); // Set up read-only user for later
db.getSiblingDB('admin').addUser("admin", "password"); // activate auth even though we're on localhost

runTests( db );

st.stop();