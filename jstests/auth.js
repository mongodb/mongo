

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

