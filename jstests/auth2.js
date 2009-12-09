db2 = new Mongo( db.getMongo().host ).getDB( db.getName() );

users = db2.getCollection( "system.users" );
users.remove( {} );

pass = "a" + Math.random();
//print( "password [" + pass + "]" );

db2.addUser( "eliot" , pass );

assert.commandFailed( db2.runCommand( { authenticate: 1, user: "eliot", nonce: "foo", key: "bar" } ) );
