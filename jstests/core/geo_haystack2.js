
t = db.geo_haystack2
t.drop()

function distance( a , b ){
    var x = a[0] - b[0];
    var y = a[1] - b[1];
    return Math.sqrt( ( x * x ) + ( y * y ) );
}

function distanceTotal( a , arr , f ){
    var total = 0;
    for ( var i=0; i<arr.length; i++ ){
        total += distance( a , arr[i][f] );
    }
    return total;
}

queries = [
    { near : [ 7 , 8 ]  , maxDistance : 3 , search : { z : 3 } } ,
]

answers = queries.map( function(){ return { totalDistance : 0 , results : [] }; } )


n = 0;
for ( x=0; x<20; x++ ){
    for ( y=0; y<20; y++ ){
        t.insert( { _id : n , loc : [ x , y ] , z : [ n % 10 , ( n + 5 ) % 10 ] } );
        
        for ( i=0; i<queries.length; i++ ){
            var d = distance( queries[i].near , [ x , y ] )
            if ( d > queries[i].maxDistance )
                continue;
            if ( queries[i].search.z != n % 10 &&
                 queries[i].search.z != ( n + 5 ) % 10 )
                continue;
            answers[i].results.push( { _id : n , loc : [ x , y ] } )
            answers[i].totalDistance += d;
        }

        n++;
    }
}

t.ensureIndex( { loc : "geoHaystack" , z : 1 } , { bucketSize : .7 } );

for ( i=0; i<queries.length; i++ ){
    print( "---------" );
    printjson( queries[i] );
    res = t.runCommand( "geoSearch" , queries[i] )
    print( "\t" + tojson( res.stats ) );
    print( "\tshould have: " + answers[i].results.length + "\t actually got: " + res.stats.n );
    assert.eq( answers[i].results.length , res.stats.n, "num:"+ i + " number matches" )
    assert.eq( answers[i].totalDistance , distanceTotal( queries[i].near , res.results , "loc" ), "num:"+ i + " totalDistance" )
    //printjson( res );    
    //printjson( answers[i].length );
}


