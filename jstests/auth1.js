db.removeAllUsers();

pass = "a" + Math.random();
//print( "password [" + pass + "]" );

db.addUser( "eliot" , pass, jsTest.basicUserRoles, 1 );

assert( db.auth( "eliot" , pass ) , "auth failed" );
assert( ! db.auth( "eliot" , pass + "a" ) , "auth should have failed" );

pass2 = "b" + Math.random();
db.changeUserPassword("eliot", pass2);

assert( ! db.auth( "eliot" , pass ) , "failed to change password failed" );
assert( db.auth( "eliot" , pass2 ) , "new password didn't take" );

assert( db.auth( "eliot" , pass2 ) , "what?" );
db.removeUser( "eliot" );
assert( ! db.auth( "eliot" , pass2 ) , "didn't remove user" );


var a = db.getMongo().getDB( "admin" );
a.removeAllUsers();
pass = "c" + Math.random();
a.addUser( "super", pass, jsTest.adminUserRoles, 1 );
assert( a.auth( "super" , pass ) , "auth failed" );
assert( !a.auth( "super" , pass + "a" ) , "auth should have failed" );

db.removeAllUsers();
pass = "a" + Math.random();

db.addUser( "eliot" , pass, jsTest.basicUserRoles, 1 );

assert.commandFailed( db.runCommand( { authenticate: 1, user: "eliot", nonce: "foo", key: "bar" } ) );

// check sanity check SERVER-3003

before = a.system.users.count()

assert.throws( function(){
    db.addUser( "" , "abc", jsTest.basicUserRoles, 1 )
} , null , "C1" )
assert.throws( function(){
    db.addUser( "abc" , "", jsTest.basicUserRoles, 1 )
} , null , "C2" )


after = a.system.users.count()
assert( before > 0 , "C3" )
assert.eq( before , after , "C4" )

// Clean up after ourselves so other tests using authentication don't get messed up.
db.removeAllUsers()
db.getSiblingDB('admin').removeAllUsers();
