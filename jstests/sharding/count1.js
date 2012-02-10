// count1.js

s = new ShardingTest( "count1" , 2 , 1 );
db = s.getDB( "test" );

// Stop balancer since doing manual stuff
// Make sure we totally stop here, otherwise balancing round can intermittently slip by
s.stopBalancer();

db.bar.save( { n : 1 } )
db.bar.save( { n : 2 } )
db.bar.save( { n : 3 } )

assert.eq( 3 , db.bar.find().count() , "bar 1" );
assert.eq( 1 , db.bar.find( { n : 1 } ).count() , "bar 2" );

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo" , key : { name : 1 } } );

primary = s.getServer( "test" ).getDB( "test" );
secondary = s.getOther( primary ).getDB( "test" );

assert.eq( 1 , s.config.chunks.count() , "sanity check A" );

db.foo.save( { _id : 1 , name : "eliot" } )
db.foo.save( { _id : 2 , name : "sara" } )
db.foo.save( { _id : 3 , name : "bob" } )
db.foo.save( { _id : 4 , name : "joe" } )
db.foo.save( { _id : 5 , name : "mark" } )
db.foo.save( { _id : 6 , name : "allan" } )

assert.eq( 6 , db.foo.find().count() , "basic count" );

s.adminCommand( { split : "test.foo" , find : { name : "joe" } } ); // [Minkey -> allan) , * [allan -> ..)
s.adminCommand( { split : "test.foo" , find : { name : "joe" } } ); // * [allan -> sara) , [sara -> Maxkey)
s.adminCommand( { split : "test.foo" , find : { name : "joe" } } ); // [alan -> joe) , [joe -> sara]

s.printChunks()

assert.eq( 6 , db.foo.find().count() , "basic count after split " );
assert.eq( 6 , db.foo.find().sort( { name : 1 } ).count() , "basic count after split sorted " );

s.adminCommand( { movechunk : "test.foo" , find : { name : "allan" } , to : secondary.getMongo().name } );

// Eventually restart balancer
s.setBalancer( true )

assert.eq( 3 , primary.foo.find().toArray().length , "primary count" );
assert.eq( 3 , secondary.foo.find().toArray().length , "secondary count" );
assert.eq( 3 , primary.foo.find().sort( { name : 1 } ).toArray().length , "primary count sorted" );
assert.eq( 3 , secondary.foo.find().sort( { name : 1 } ).toArray().length , "secondary count sorted" );

assert.eq( 6 , db.foo.find().toArray().length , "total count after move" );
assert.eq( 6 , db.foo.find().sort( { name : 1 } ).toArray().length , "total count() sorted" );

assert.eq( 6 , db.foo.find().sort( { name : 1 } ).count() , "total count with count() after move" );

function nameString( c ){
    var s = "";
    while ( c.hasNext() ){
        var o = c.next();
        if ( s.length > 0 )
            s += ",";
        s += o.name;
    }
    return s;
}

assert.eq( "allan,bob,eliot,joe,mark,sara" ,  nameString( db.foo.find().sort( { name : 1 } ) ) , "sort 1" );
assert.eq( "sara,mark,joe,eliot,bob,allan" ,  nameString( db.foo.find().sort( { name : -1 } ) ) , "sort 2" );

assert.eq( 2 , db.foo.find().limit(2).itcount() , "LS1" )
assert.eq( 2 , db.foo.find().skip(2).limit(2).itcount() , "LS2" )
assert.eq( 1 , db.foo.find().skip(5).limit(2).itcount() , "LS3" )
assert.eq( 6 , db.foo.find().limit(2).count() , "LSC1" )
assert.eq( 2 , db.foo.find().limit(2).size() , "LSC2" )
assert.eq( 2 , db.foo.find().skip(2).limit(2).size() , "LSC3" )
assert.eq( 1 , db.foo.find().skip(5).limit(2).size() , "LSC4" )

assert.eq( "allan,bob" , nameString( db.foo.find().sort( { name : 1 } ).limit(2) ) , "LSD1" )
assert.eq( "bob,eliot" , nameString( db.foo.find().sort( { name : 1 } ).skip(1).limit(2) ) , "LSD2" )
assert.eq( "joe,mark" , nameString( db.foo.find().sort( { name : 1 } ).skip(3).limit(2) ) , "LSD3" )

assert.eq( "eliot,sara" , nameString( db.foo.find().sort( { _id : 1 } ).limit(2) ) , "LSE1" )
assert.eq( "sara,bob" , nameString( db.foo.find().sort( { _id : 1 } ).skip(1).limit(2) ) , "LSE2" )
assert.eq( "joe,mark" , nameString( db.foo.find().sort( { _id : 1 } ).skip(3).limit(2) ) , "LSE3" )

for ( i=0; i<10; i++ ){
    db.foo.save( { _id : 7 + i , name : "zzz" + i } )
}

assert.eq( 10 , db.foo.find( { name : { $gt : "z" } } ).itcount() , "LSF1" )
assert.eq( 10 , db.foo.find( { name : { $gt : "z" } } ).sort( { _id : 1 } ).itcount() , "LSF2" )
assert.eq( 5 , db.foo.find( { name : { $gt : "z" } } ).sort( { _id : 1 } ).skip(5).itcount() , "LSF3" )
sleep( 5000 )
assert.eq( 3 , db.foo.find( { name : { $gt : "z" } } ).sort( { _id : 1 } ).skip(5).limit(3).itcount() , "LSF4" )

// SERVER-3567
assert.eq( 2 , db.foo.find().limit(2).itcount() , "N1" );
assert.eq( 2 , db.foo.find().limit(-2).itcount() , "N2" );
assert.eq( 2 , db.foo.find().skip(4).limit(2).itcount() , "N3" );
assert.eq( 2 , db.foo.find().skip(4).limit(-2).itcount() , "N4" );

s.stop();


