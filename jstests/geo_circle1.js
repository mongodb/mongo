
t = db.geo_circle1;
t.drop();

for ( x=0; x<=20; x++ )
    for ( y=0; y<=20; y++ )
        t.save( { loc : [ x , y ] } )

t.ensureIndex( { loc : "2d" } );

q = { loc : { $within : { $center : [ [ 5 , 5 ] , 6 ] } } }
//t.find( q ).forEach( printjson )



    
