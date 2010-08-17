
t = db.geo_center_sphere1;
t.drop();

searches = [ 
    //  y , x    rad
    [ [ 5 , 5 ] , 0.03 ] ,
    [ [ 5 , 5 ] , 0.01 ] ,
    [ [ 5 , 5 ] , 0.05 ] ,
    [ [ 0 , 5 ] , 0.05 ] ,
    [ [ 5 , 70 ] , 0.05 ] ,
    [ [ 5 , -70 ] , 0.05 ] ,
    [ [ 135 , 70 ] , 0.05 ] ,
    [ [ 135 , -70 ] , 0.05 ] ,
];
correct = searches.map( function(z){ return []; } );

num = 0;

for ( x=-179; x<=179; x++ ){
    for ( y=-89; y<=89; y++ ){
        o = { _id : num++ , loc : [ x , y ] } 
        t.save( o )
        for ( i=0; i<searches.length; i++ ){
            if ( Geo.sphereDistance( [ x , y ] , searches[i][0] ) <= searches[i][1] )
                correct[i].push( o );
        }
    }
}

t.ensureIndex( { loc : "2d" } );

for ( i=0; i<searches.length; i++ ){
    //print( tojson( searches[i] ) + "\t" + correct[i].length )
    q = { loc : { $within : { $centerSphere : searches[i] } } }

    //correct[i].forEach( printjson )
    //printjson( q );
    //t.find( q ).forEach( printjson )
    
    //printjson(t.find( q ).explain())

    //printjson( Array.sort( correct[i].map( function(z){ return z._id; } ) ) )
    //printjson( Array.sort( t.find(q).map( function(z){ return z._id; } ) ) )
    
    //print(correct[i].length);
    assert.eq( correct[i].length , t.find( q ).itcount() , "itcount : " + tojson( searches[i] ) );
    assert.eq( correct[i].length , t.find( q ).count() , "count : " + tojson( searches[i] ) );
    assert.gt( correct[i].length * 2 , t.find(q).explain().nscanned , "nscanned : " + tojson( searches[i] ) )
}








    
