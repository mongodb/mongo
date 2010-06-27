
t = db.geo_search1
t.drop()

function distance( a , b ){
    var x = a[0] - b[0];
    var y = a[1] - b[1];
    return Math.sqrt( ( x * x ) + ( y * y ) );
}

queries = [
    { near : [ 7 , 8 ]  , maxDistance : 3 , search : { z : 3 } } ,
]

answers = queries.map( function(){ return[]; } )

n = 0;
for ( x=0; x<20; x++ ){
    for ( y=0; y<20; y++ ){
        t.insert( { _id : n , loc : [ x , y ] , z : n % 5 } );
        
        for ( i=0; i<queries.length; i++ ){
            if ( distance( queries[i].near , [ x , y ] ) > queries[i].maxDistance )
                continue;
            if ( queries[i].search.z != n % 5 )
                continue;
            answers[i].push( { _id : n , loc : [ x , y ]} )
        }

        n++;
    }
}

t.ensureIndex( { loc : "geosearch" , z : 1 } , { bucketSize : .7 } );

for ( i=0; i<queries.length; i++ ){
    print( "---------" );
    printjson( queries[i] );
    res = t.runCommand( "geoSearch" , queries[i] )
    print( "\t" + tojson( res.stats ) );
    print( "\tshould have: " + answers[i].length + "\t actually got: " + res.stats.n );
    
    assert.eq( answers[i].length , res.stats.n, "num:"+ i + " number matches" )
    printjson( res );    
    printjson( answers[i].length );
}


