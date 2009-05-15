baseName = "jstests_shellspawn";
t = db.getCollection( baseName );
t.drop();

spawn = startMongoProgramNoConnect( "mongo", "--port", myPort(), "--eval", "sleep( 2000 ); db.getCollection( \"" + baseName + "\" ).save( {a:1} );" );

assert.soon( function() { return 1 == t.count(); } );
