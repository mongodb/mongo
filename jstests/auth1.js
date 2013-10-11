db.dropAllUsers();

pass = "a" + Math.random();
//print( "password [" + pass + "]" );

db.addUser({user: "eliot" ,pwd:  pass, roles: jsTest.basicUserRoles});

assert( db.auth( "eliot" , pass ) , "auth failed" );
assert( ! db.auth( "eliot" , pass + "a" ) , "auth should have failed" );

pass2 = "b" + Math.random();
db.changeUserPassword("eliot", pass2);

assert( ! db.auth( "eliot" , pass ) , "failed to change password failed" );
assert( db.auth( "eliot" , pass2 ) , "new password didn't take" );

assert( db.auth( "eliot" , pass2 ) , "what?" );
db.dropUser( "eliot" );
assert( ! db.auth( "eliot" , pass2 ) , "didn't drop user" );


var a = db.getMongo().getDB( "admin" );
a.dropAllUsers();
pass = "c" + Math.random();
a.addUser({user: "super", pwd: pass, roles: jsTest.adminUserRoles});
assert( a.auth( "super" , pass ) , "auth failed" );
assert( !a.auth( "super" , pass + "a" ) , "auth should have failed" );

db.dropAllUsers();
pass = "a" + Math.random();

db.addUser({user: "eliot" , pwd: pass, roles: jsTest.basicUserRoles});

assert.commandFailed( db.runCommand( { authenticate: 1, user: "eliot", nonce: "foo", key: "bar" } ) );

// check sanity check SERVER-3003

before = a.system.users.count()

assert.throws( function(){
    db.addUser({ user: "" , pwd: "abc", roles: jsTest.basicUserRoles});
} , null , "C1" )
assert.throws( function(){
    db.addUser({ user: "abc" , pwd: "", roles: jsTest.basicUserRoles});
} , null , "C2" )


after = a.system.users.count()
assert( before > 0 , "C3" )
assert.eq( before , after , "C4" )

// Clean up after ourselves so other tests using authentication don't get messed up.
db.dropAllUsers()
db.getSiblingDB('admin').dropAllUsers();
