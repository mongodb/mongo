
t = db.geo2
t.drop();

n = 1
for ( var x=-100; x<100; x+=2 ){
    for ( var y=-100; y<100; y+=2 ){
        t.insert( { _id : n++ , loc : [ x , y ] } )
    }
}

t.ensureIndex( { loc : "2d" } )

fast = db.runCommand( { geo2d : t.getName() , near : [ 50 , 50 ] , num : 10 } );
slow = db.runCommand( { geo2d : t.getName() , near : [ 50 , 50 ] , num : 10 , start : "11" } );

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
        total += cur.next()["$distance"];
        outof++;
    }
    return total/outof;
}

assert.eq( fast.stats.avgDistance , a( t.find( { loc : { $near : [ 50 , 50 ] } } ).limit(10) ) , "B1" )

print( "---" )
query = t.find( { loc : { $near : [ 50 , 50 ] } } ).limit(10)
query.forEach( printjson )

print( "---" )
query = t.find( { loc : { $near : [ 50 , 50 ] } } ).limit(3)
query.forEach( printjson )

