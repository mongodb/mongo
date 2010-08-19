
t = db.geo_circle1;
t.drop();

searches = [ 
    [ [ 5 , 5 ] , 3 ] ,
    [ [ 5 , 5 ] , 1 ] ,
    [ [ 5 , 5 ] , 5 ] ,
    [ [ 0 , 5 ] , 5 ] ,
];
correct = searches.map( function(z){ return []; } );

num = 0;

for ( x=0; x<=20; x++ ){
    for ( y=0; y<=20; y++ ){
        o = { _id : num++ , loc : [ x , y ] } 
        t.save( o )
        for ( i=0; i<searches.length; i++ )
            if ( Geo.distance( [ x , y ] , searches[i][0] ) <= searches[i][1] )
                correct[i].push( o );
    }
}

t.ensureIndex( { loc : "2d" } );

for ( i=0; i<searches.length; i++ ){
    //print( tojson( searches[i] ) + "\t" + correct[i].length )
    q = { loc : { $within : { $center : searches[i] } } }

    //correct[i].forEach( printjson )
    //printjson( q );
    //t.find( q ).forEach( printjson )

    //printjson( Array.sort( correct[i].map( function(z){ return z._id; } ) ) )
    //printjson( Array.sort( t.find(q).map( function(z){ return z._id; } ) ) )
    
    assert.eq( correct[i].length , t.find( q ).itcount() , "itcount : " + tojson( searches[i] ) );
    assert.eq( correct[i].length , t.find( q ).count() , "count : " + tojson( searches[i] ) );
    assert.gt( correct[i].length * 2 , t.find(q).explain().nscanned , "nscanned : " + tojson( searches[i] ) )
}








    
