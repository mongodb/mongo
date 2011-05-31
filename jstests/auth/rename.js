// test renameCollection with auth

port = allocatePorts( 1 )[ 0 ];

baseName = "jstests_rename_auth";
m = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface" );

db1 = m.getDB( baseName )
db2 = m.getDB( baseName + '_other' )
admin = m.getDB( 'admin' )

// auth not yet checked since we are on localhost
db1.addUser( "foo", "bar" );
db2.addUser( "bar", "foo" );

printjson(db1.a.count());
db1.a.save({});
assert.eq(db1.a.count(), 1);

//this makes auth required on localhost
admin.addUser('not', 'used');

// can't run same db w/o auth
assert.commandFailed( admin.runCommand({renameCollection:db1.a.getFullName(), to: db1.b.getFullName()}) );

// can run same db with auth
db1.auth('foo', 'bar')
assert.commandWorked( admin.runCommand({renameCollection:db1.a.getFullName(), to: db1.b.getFullName()}) );

// can't run diff db w/o auth
assert.commandFailed( admin.runCommand({renameCollection:db1.b.getFullName(), to: db2.a.getFullName()}) );

// can run diff db with auth
db2.auth('bar', 'foo');
assert.commandWorked( admin.runCommand({renameCollection:db1.b.getFullName(), to: db2.a.getFullName()}) );

// test post conditions
assert.eq(db1.a.count(), 0);
assert.eq(db1.b.count(), 0);
assert.eq(db2.a.count(), 1);
