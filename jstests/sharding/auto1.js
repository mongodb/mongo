// auto1.js

s = new ShardingTest( "auto1" , 2 , 1 , 1, { enableBalancer : 1 } );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );

bigString = "";
while ( bigString.length < 1024 * 50 )
    bigString += "asocsancdnsjfnsdnfsjdhfasdfasdfasdfnsadofnsadlkfnsaldknfsad";

db = s.getDB( "test" )
coll = db.foo;

var i=0;

var bulk = coll.initializeUnorderedBulkOp();
for ( ; i<100; i++ ){
    bulk.insert( { num : i , s : bigString } );
}
assert.writeOK( bulk.execute() );

primary = s.getServer( "test" ).getDB( "test" );

counts = []

s.printChunks();
counts.push( s.config.chunks.count() );
assert.eq(100, db.foo.find().itcount());

print( "datasize: " + tojson( s.getServer( "test" ).getDB( "admin" ).runCommand( { datasize : "test.foo" } ) ) );

bulk = coll.initializeUnorderedBulkOp();
for ( ; i<200; i++ ){
    bulk.insert( { num : i , s : bigString } );
}
assert.writeOK( bulk.execute() );

s.printChunks()
s.printChangeLog()
counts.push( s.config.chunks.count() );

bulk = coll.initializeUnorderedBulkOp();
for ( ; i<400; i++ ){
    bulk.insert( { num : i , s : bigString } );
}
assert.writeOK( bulk.execute() );

s.printChunks();
s.printChangeLog()
counts.push( s.config.chunks.count() );

bulk = coll.initializeUnorderedBulkOp();
for ( ; i<700; i++ ){
    bulk.insert( { num : i , s : bigString } );
}
assert.writeOK( bulk.execute() );

s.printChunks();
s.printChangeLog()
counts.push( s.config.chunks.count() );

assert( counts[counts.length-1] > counts[0] , "counts 1 : " + tojson( counts ) )
sorted = counts.slice(0)
// Sort doesn't sort numbers correctly by default, resulting in fail
sorted.sort( function(a, b){ return a - b } )
assert.eq( counts , sorted , "counts 2 : " + tojson( counts ) )

print( counts )

printjson( db.stats() )

s.stop();
