// key_many.js

// values have to be sorted
types = 
    [ { name : "string" , values : [ "allan" , "bob" , "eliot" , "joe" , "mark" , "sara" ] } ,
      { name : "double" , values : [ "1.2" , "3.5" , "4.5" , "4.6" , "6.7" , "9.9" ] , keyfield : "a" } ,
      { name : "string_id" , values : [ "allan" , "bob" , "eliot" , "joe" , "mark" , "sara" ] , keyfield : "_id" } ]

s = new ShardingTest( "key_many" , 2 );

s.adminCommand( { enablesharding : "test" } )
db = s.getDB( "test" );
primary = s.getServer( "test" ).getDB( "test" );
seconday = s.getOther( primary ).getDB( "test" );

function makeObject( v ){
    if ( ! curT.keyfield )
        return { k : v };
    var o = {};
    o[curT.keyfield] = v;
    return o;
}

function getKey( o ){
    var mykey = curT.keyfield || "k";
    return o[mykey];
}



for ( var i=0; i<types.length; i++ ){
    curT = types[i];
    
    var shortName = "foo_" + curT.name;
    var longName = "test." + shortName;
    
    var c = db[shortName]
    s.adminCommand( { shardcollection : longName , key : makeObject( 1 ) } );
    
    assert.eq( 1 , s.config.chunks.find( { ns : longName } ).count() , curT.name + " sanity check A" );

    var unsorted = Array.shuffle( Object.extend( [] , curT.values ) );
    for ( var x=0; x<unsorted.length; x++ )
        c.save( makeObject( unsorted[x] ) );
    
    assert.eq( 6 , c.find().count() , curT.name + " basic count" );
    
    s.adminCommand( { split : longName , find : makeObject( curT.values[3] ) } );
    s.adminCommand( { split : longName , find : makeObject( curT.values[3] ) } );
    s.adminCommand( { split : longName , find : makeObject( curT.values[3] ) } );

    s.adminCommand( { movechunk : longName , find : makeObject( curT.values[3] ) , to : seconday.getMongo().name } );
    
    s.printChunks();
    
    assert.eq( 3 , primary[shortName].find().toArray().length , curT.name + " primary count" );
    assert.eq( 3 , seconday[shortName].find().toArray().length , curT.name + " secondary count" );
    
    assert.eq( 6 , c.find().toArray().length , curT.name + " total count" );
    assert.eq( 6 , c.find().sort( makeObject( 1 ) ).toArray().length , curT.name + " total count sorted" );
    
    assert.eq( 6 , c.find().sort( makeObject( 1 ) ).count() , curT.name + " total count with count()" );
    
    assert.eq( curT.values , c.find().sort( makeObject( 1 ) ).toArray().map( getKey ) , curT.name + " sort 1" );
    assert.eq( curT.values.reverse() , c.find().sort( makeObject( -1 ) ).toArray().map( getKey ) , curT.name + " sort 2" );

    assert.eq( 0 , c.find( { xx : 17 } ).sort( { zz : 1 } ).count() , curT.name + " xx 0a " );
    assert.eq( 0 , c.find( { xx : 17 } ).sort( makeObject( 1 ) ).count() , curT.name + " xx 0b " );
    assert.eq( 0 , c.find( { xx : 17 } ).count() , curT.name + " xx 0c " );
    assert.eq( 0 , c.find( { xx : { $exists : true } } ).count() , curT.name + " xx 1 " );
    c.update( makeObject( curT.values[3] ) , { $set : { xx : 17 } } );
    assert.eq( 1 , c.find( { xx : { $exists : true } } ).count() , curT.name + " xx 2 " );
    assert.eq( curT.values[3] , getKey( c.findOne( { xx : 17 } ) ) , curT.name + " xx 3 " );

    // TODO multi update

    // TODO remove
}

  
s.stop();


