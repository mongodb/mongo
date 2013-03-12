
t = db.stats1;
t.drop();

t.save( { a : 1 } );

assert.lt( 0 , t.dataSize() , "A" );
assert.lt( t.dataSize() , t.storageSize() , "B" );
assert.lt( 0 , t.totalIndexSize() , "C" );

var stats = db.stats();
assert.gt( stats.fileSize, 0 );
assert.eq( stats.dataFileVersion.major, 4 );
assert.eq( stats.dataFileVersion.minor, 5 );

db.getSiblingDB( "emptydatabase" ).dropDatabase();
var statsEmptyDB = db.getSiblingDB( "emptydatabase" ).stats();
assert.eq( statsEmptyDB.fileSize, 0 );
assert.eq( {}, statsEmptyDB.dataFileVersion );

