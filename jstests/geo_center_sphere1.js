
t = db.geo_center_sphere1;
t.drop();

skip = 3 // lower for more rigor, higher for more speed (tested with .5, .678, 1, 2, 3, and 4)

searches = [ 
    //  x , y    rad
    [ [ 5 , 0 ] , 0.05 ] , // ~200 miles
    [ [ 135 , 0 ] , 0.05 ] ,

    [ [ 5 , 70 ] , 0.05 ] ,
    [ [ 135 , 70 ] , 0.05 ] ,
    [ [ 5 , 85 ] , 0.05 ] ,

    [ [ 20 , 0 ] , 0.25 ] , // ~1000 miles
    [ [ 20 , -45 ] , 0.25 ] ,
    [ [ -20 , 60 ] , 0.25 ] ,
    [ [ -20 , -70 ] , 0.25 ] ,
];
correct = searches.map( function(z){ return []; } );

num = 0;

for ( x=-179; x<=179; x += skip ){
    for ( y=-89; y<=89; y += skip ){
        o = { _id : num++ , loc : [ x , y ] } 
        t.save( o )
        for ( i=0; i<searches.length; i++ ){
            if ( Geo.sphereDistance( [ x , y ] , searches[i][0] ) <= searches[i][1])
                correct[i].push( o );
        }
    }
    gc(); // needed with low skip values
}

t.ensureIndex( { loc : "2d" } );

for ( i=0; i<searches.length; i++ ){
    print('------------');
    print( tojson( searches[i] ) + "\t" + correct[i].length )
    q = { loc : { $within : { $centerSphere : searches[i] } } }

    //correct[i].forEach( printjson )
    //printjson( q );
    //t.find( q ).forEach( printjson )
    
    //printjson(t.find( q ).explain())

    //printjson( Array.sort( correct[i].map( function(z){ return z._id; } ) ) )
    //printjson( Array.sort( t.find(q).map( function(z){ return z._id; } ) ) )
    
    var numExpected = correct[i].length
    var x = correct[i].map( function(z){ return z._id; } )
    var y = t.find(q).map( function(z){ return z._id; } )

    missing = [];
    epsilon = 0.001; // allow tenth of a percent error due to conversions
    for (var j=0; j<x.length; j++){
        if (!Array.contains(y, x[j])){
            missing.push(x[j]);
            var obj = t.findOne({_id: x[j]});
            var dist = Geo.sphereDistance(searches[i][0], obj.loc);
            print("missing: " + tojson(obj) + " " + dist)
            if ((Math.abs(dist - searches[i][1]) / dist) < epsilon)
                numExpected -= 1;
        }
    }
    for (var j=0; j<y.length; j++){
        if (!Array.contains(x, y[j])){
            missing.push(y[j]);
            var obj = t.findOne({_id: y[j]});
            var dist = Geo.sphereDistance(searches[i][0], obj.loc);
            print("extra: " + tojson(obj) + " " + dist)
            if ((Math.abs(dist - searches[i][1]) / dist) < epsilon)
                numExpected += 1;
        }
    }


    assert.eq( numExpected , t.find( q ).itcount() , "itcount : " + tojson( searches[i] ) );
    assert.eq( numExpected , t.find( q ).count() , "count : " + tojson( searches[i] ) );
    assert.gt( numExpected * 2 , t.find(q).explain().nscanned , "nscanned : " + tojson( searches[i] ) )
}








    
