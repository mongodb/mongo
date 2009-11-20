// key_many.js

// values have to be sorted
types = 
    [ { name : "string" , values : [ "allan" , "bob" , "eliot" , "joe" , "mark" , "sara" ] , keyfield: "k" } ,
      { name : "double" , values : [ 1.2 , 3.5 , 4.5 , 4.6 , 6.7 , 9.9 ] , keyfield : "a" } ,
      { name : "string_id" , values : [ "allan" , "bob" , "eliot" , "joe" , "mark" , "sara" ] , keyfield : "_id" },
      { name : "embedded" , values : [ "allan" , "bob" , "eliot" , "joe" , "mark" , "sara" ] , keyfield : "a.b" } ,
      { name : "embedded 2" , values : [ "allan" , "bob" , "eliot" , "joe" , "mark" , "sara" ] , keyfield : "a.b.c" } ,
    ]

s = new ShardingTest( "key_many" , 2 );

s.adminCommand( { enablesharding : "test" } )
db = s.getDB( "test" );
primary = s.getServer( "test" ).getDB( "test" );
seconday = s.getOther( primary ).getDB( "test" );

function makeObjectDotted( v ){
    var o = {};
    o[curT.keyfield] = v;
    return o;
}

function makeObject( v ){
    var o = {};
    var p = o;

    var keys = curT.keyfield.split('.');
    for(var i=0; i<keys.length-1; i++){
        p[keys[i]] = {};
        p = p[keys[i]];
    }

    p[keys[i]] = v;

    return o;
}

function getKey( o ){
    var keys = curT.keyfield.split('.');
    for(var i=0; i<keys.length; i++){
        o = o[keys[i]];
    }
    return o;
}



for ( var i=0; i<types.length; i++ ){
    curT = types[i]; //global

    print("\n\n#### Now Testing " + curT.name + " ####\n\n");

    var shortName = "foo_" + curT.name;
    var longName = "test." + shortName;
    
    var c = db[shortName];
    s.adminCommand( { shardcollection : longName , key : makeObjectDotted( 1 ) } );
    
    assert.eq( 1 , s.config.chunks.find( { ns : longName } ).count() , curT.name + " sanity check A" );

    var unsorted = Array.shuffle( Object.extend( [] , curT.values ) );
    c.insert( makeObject( unsorted[0] ) );
    for ( var x=1; x<unsorted.length; x++ )
        c.save( makeObject( unsorted[x] ) );
    
    assert.eq( 6 , c.find().count() , curT.name + " basic count" );
    
    s.adminCommand( { split : longName , find : makeObjectDotted( curT.values[3] ) } );
    s.adminCommand( { split : longName , find : makeObjectDotted( curT.values[3] ) } );
    s.adminCommand( { split : longName , find : makeObjectDotted( curT.values[3] ) } );

    s.adminCommand( { movechunk : longName , find : makeObjectDotted( curT.values[3] ) , to : seconday.getMongo().name } );
    
    s.printChunks();
    
    assert.eq( 3 , primary[shortName].find().toArray().length , curT.name + " primary count" );
    assert.eq( 3 , seconday[shortName].find().toArray().length , curT.name + " secondary count" );
    
    assert.eq( 6 , c.find().toArray().length , curT.name + " total count" );
    assert.eq( 6 , c.find().sort( makeObjectDotted( 1 ) ).toArray().length , curT.name + " total count sorted" );
    
    assert.eq( 6 , c.find().sort( makeObjectDotted( 1 ) ).count() , curT.name + " total count with count()" );

    assert.eq( curT.values , c.find().sort( makeObjectDotted( 1 ) ).toArray().map( getKey ) , curT.name + " sort 1" );
    assert.eq( curT.values.reverse() , c.find().sort( makeObjectDotted( -1 ) ).toArray().map( getKey ) , curT.name + " sort 2" );


    assert.eq( 0 , c.find( { xx : 17 } ).sort( { zz : 1 } ).count() , curT.name + " xx 0a " );
    assert.eq( 0 , c.find( { xx : 17 } ).sort( makeObjectDotted( 1 ) ).count() , curT.name + " xx 0b " );
    assert.eq( 0 , c.find( { xx : 17 } ).count() , curT.name + " xx 0c " );
    assert.eq( 0 , c.find( { xx : { $exists : true } } ).count() , curT.name + " xx 1 " );

    c.update( makeObjectDotted( curT.values[3] ) , { $set : { xx : 17 } } );
    assert.eq( 1 , c.find( { xx : { $exists : true } } ).count() , curT.name + " xx 2 " );
    assert.eq( curT.values[3] , getKey( c.findOne( { xx : 17 } ) ) , curT.name + " xx 3 " );

    c.ensureIndex( { _id : 1 } , { unique : true } );
    assert.eq( null , db.getLastError() , curT.name + " creating _id index should be ok" );
    
    // multi update
    var mysum = 0;
    c.find().forEach( function(z){ mysum += z.xx || 0; } );
    assert.eq( 17 , mysum, curT.name + " multi update pre" );
    c.update( {} , { $inc : { xx : 1 } } , false , true );
    var mysum = 0;
    c.find().forEach( function(z){ mysum += z.xx || 0; } );
    assert.eq( 23 , mysum, curT.name + " multi update" );
    
    // TODO remove
}

  
s.stop();


