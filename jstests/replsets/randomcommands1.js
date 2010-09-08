
replTest = new ReplSetTest( {name: 'randomcommands1', nodes: 3} );

nodes = replTest.startSet();
replTest.initiate();

master = replTest.getMaster();
slaves = replTest.liveNodes.slaves;
printjson(replTest.liveNodes);

db = master.getDB("foo")
t = db.foo

ts = slaves.map( function(z){ z.setSlaveOk(); return z.getDB( "foo" ).foo; } )

t.save({a: 1000});
t.ensureIndex( { a : 1 } )

db.getLastError( 3 , 30000 )

ts.forEach( function(z){ assert.eq( 2 , z.getIndexKeys().length , "A " + z.getMongo() ); } )

t.reIndex()

db.getLastError( 3 , 30000 )
ts.forEach( function(z){ assert.eq( 2 , z.getIndexKeys().length , "A " + z.getMongo() ); } )

replTest.stopSet( 15 )

