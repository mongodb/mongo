users = db.getCollection( "system.users" );
users.remove( {} );

pass = "a" + Math.random();
//print( "password [" + pass + "]" );

db.addUser( "eliot" , pass );

assert( db.auth( "eliot" , pass ) , "auth failed" );
assert( ! db.auth( "eliot" , pass + "a" ) , "auth should have failed" );

pass2 = "b" + Math.random();
db.addUser( "eliot" , pass2 );

assert( ! db.auth( "eliot" , pass ) , "failed to change password failed" );
assert( db.auth( "eliot" , pass2 ) , "new password didn't take" );

assert( db.auth( "eliot" , pass2 ) , "what?" );
db.removeUser( "eliot" );
assert( ! db.auth( "eliot" , pass2 ) , "didn't remove user" );


var a = db.getMongo().getDB( "admin" );
users = a.getCollection( "system.users" );
users.remove( {} );
pass = "c" + Math.random();
a.addUser( "super", pass );
assert( a.auth( "super" , pass ) , "auth failed" );
assert( !a.auth( "super" , pass + "a" ) , "auth should have failed" );

users = db.getCollection( "system.users" );
users.remove( {} );
pass = "a" + Math.random();

db.addUser( "eliot" , pass );

assert.commandFailed( db.runCommand( { authenticate: 1, user: "eliot", nonce: "foo", key: "bar" } ) );

// check sanity check SERVER-3003

before = db.system.users.count()

assert.throws( function(){
    db.addUser( "" , "abc" )
} , null , "C1" )
assert.throws( function(){
    db.addUser( "abc" , "" )
} , null , "C2" )


after = db.system.users.count()
assert( before > 0 , "C3" )
assert.eq( before , after , "C4" )

// Clean up after ourselves so other tests using authentication don't get messed up.
db.system.users.remove({})
db.getSiblingDB('admin').system.users.remove({})
