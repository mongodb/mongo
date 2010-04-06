
t = db.geo2
t.drop();

n = 1
for ( var x=-100; x<100; x+=2 ){
    for ( var y=-100; y<100; y+=2 ){
        t.insert( { _id : n++ , loc : [ x , y ] } )
    }
}

t.ensureIndex( { loc : "2d" } )

fast = db.runCommand( { geoNear : t.getName() , near : [ 50 , 50 ] , num : 10 } );
slow = db.runCommand( { geoNear : t.getName() , near : [ 50 , 50 ] , num : 10 , start : "11" } );

printjson(fast.stats);
printjson(slow.stats);

v = "\n" + tojson( fast ) + "\n" + tojson( slow );

assert.lt( fast.stats.nscanned * 10 , slow.stats.nscanned , "A1" + v );
assert.lt( fast.stats.objectsLoaded , slow.stats.objectsLoaded , "A2" + v );
assert.eq( fast.stats.avgDistance , slow.stats.avgDistance , "A3" + v );

function a( cur ){
    var total = 0;
    var outof = 0;
    while ( cur.hasNext() ){
        var o = cur.next();
        total += Geo.distance( [ 50 , 50 ] , o.loc );
        outof++;
    }
    return total/outof;
}

assert.close( fast.stats.avgDistance , a( t.find( { loc : { $near : [ 50 , 50 ] } } ).limit(10) ) , "B1" )
assert.close( 1.33333 , a( t.find( { loc : { $near : [ 50 , 50 ] } } ).limit(3) ) , "B2" );
assert.close( fast.stats.avgDistance , a( t.find( { loc : { $near : [ 50 , 50 ] } } ).limit(10) ) , "B3" );

printjson( t.find( { loc : { $near : [ 50 , 50 ] } } ).explain() )


assert.lt( 3 , a( t.find( { loc : { $near : [ 50 , 50 ] } } ).limit(50) ) , "C1" )
assert.gt( 3 , a( t.find( { loc : { $near : [ 50 , 50 , 3 ] } } ).limit(50) ) , "C2" )
assert.gt( 3 , a( t.find( { loc : { $near : [ 50 , 50 ] , $maxDistance : 3 } } ).limit(50) ) , "C3" )



