
t = db.geo3
t.drop();

n = 1
for ( var x=-100; x<100; x+=2 ){
    for ( var y=-100; y<100; y+=2 ){
        t.insert( { _id : n++ , loc : [ x , y ] , a : Math.abs( x ) % 5 , b : Math.abs( y ) % 5 } )
    }
}

t.ensureIndex( { loc : "2d" } )

fast = db.runCommand( { geo2d : t.getName() , near : [ 50 , 50 ] , num : 10 } );
slow = db.runCommand( { geo2d : t.getName() , near : [ 50 , 50 ] , num : 10 , start : "11" } );

assert.lt( fast.stats.nscanned * 10 , slow.stats.nscanned , "A1" );
assert.lt( fast.stats.objectsLoaded , slow.stats.objectsLoaded , "A2" );
assert.eq( fast.stats.avgDistance , slow.stats.avgDistance , "A3" );
//printjson( fast.stats );

// test filter

filtered1 = db.runCommand( { geo2d : t.getName() , near : [ 50 , 50 ] , num : 10 , query : { a : 2 } } );
assert.eq( 10 , filtered1.results.length , "B1" );
filtered1.results.forEach( function(z){ assert.eq( 2 , z.obj.a , "B2: " + tojson( z ) ); } )
//printjson( filtered1.stats );

t.dropIndex( { loc : "2d" } )
t.ensureIndex( { loc : "2d" , a : 1 } )

filtered2 = db.runCommand( { geo2d : t.getName() , near : [ 50 , 50 ] , num : 10 , query : { a : 2 } } );
assert.eq( 10 , filtered2.results.length , "B3" );
filtered2.results.forEach( function(z){ assert.eq( 2 , z.obj.a , "B4: " + tojson( z ) ); } )
//printjson( filtered2.stats );

assert.eq( filtered1.stats.avgDistance , filtered2.stats.avgDistance , "C1" )
assert.eq( filtered1.stats.nscanned , filtered2.stats.nscanned , "C3" )
assert.gt( filtered1.stats.objectsLoaded , filtered2.stats.objectsLoaded , "C3" )
