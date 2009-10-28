// key_many.js

// values have to be sorted
types = 
    [ { name : "string" , values : [ "allan" , "bob" , "eliot" , "joe" , "mark" , "sara" ] } ,
      { name : "double" , values : [ "1.2" , "3.5" , "4.5" , "4.6" , "6.7" , "9.9" ] } ]; 

s = new ShardingTest( "key_many" , 2 );

s.adminCommand( { enablesharding : "test" } )
db = s.getDB( "test" );
primary = s.getServer( "test" ).getDB( "test" );
seconday = s.getOther( primary ).getDB( "test" );

for ( var i=0; i<types.length; i++ ){
    var t = types[i];
    
    var shortName = "foo_" + t.name;
    var longName = "test." + shortName;
    
    var c = db[shortName]
    s.adminCommand( { shardcollection : longName , key : { k : 1 } } );
    
    assert.eq( 1 , s.config.chunks.find( { ns : longName } ).count() , t.name + " sanity check A" );

    var unsorted = Array.shuffle( Object.extend( [] , t.values ) );
    for ( var x=0; x<unsorted.length; x++ )
        c.save( { k : unsorted[x] } );
    
    assert.eq( 6 , c.find().count() , t.name + " basic count" );
    
    s.adminCommand( { split : longName , find : { k : t.values[3] } } );
    s.adminCommand( { split : longName , find : { k : t.values[3] } } );
    s.adminCommand( { split : longName , find : { k : t.values[3] } } );
    
    s.adminCommand( { movechunk : longName , find : { k : t.values[3] } , to : seconday.getMongo().name } );
    
    s.printChunks();
    
    assert.eq( 3 , primary[shortName].find().toArray().length , t.name + " primary count" );
    assert.eq( 3 , seconday[shortName].find().toArray().length , t.name + " secondary count" );
    
    assert.eq( 6 , c.find().toArray().length , t.name + " total count" );
    assert.eq( 6 , c.find().sort( { k : 1 } ).toArray().length , t.name + " total count sorted" );
    
    assert.eq( 6 , c.find().sort( { k : 1 } ).count() , t.name + " total count with count()" );
    
    assert.eq( t.values ,  c.find().sort( { k : 1 } ).toArray().map( function(z){ return z.k; } ) , t.name + " sort 1" );
    assert.eq( t.values.reverse() ,  c.find().sort( { k : -1 } ).toArray().map( function(z){ return z.k; } ) , t.name + " sort 2" );

}

    
s.stop();


