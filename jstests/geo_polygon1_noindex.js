// SERVER-7343: allow $within without a geo index.

t = db.geo_polygon1_noindex;
t.drop();

num = 0;
for ( x=1; x < 9; x++ ){
    for ( y= 1; y < 9; y++ ){
        o = { _id : num++ , loc : [ x , y ] };
        t.save( o );
    }
}

triangle = [[0,0], [1,1], [0,2]];

// Look at only a small slice of the data within a triangle
assert.eq( 1 , t.find({ loc: { "$within": { "$polygon" : triangle }}} ).count() , "Triangle Test" );

boxBounds = [ [0,0], [0,10], [10,10], [10,0] ];

assert.eq( num , t.find( { loc : { "$within" : { "$polygon" : boxBounds } } } ).count() , "Bounding Box Test" );

//Make sure we can add object-based polygons
assert.eq( num, t.find( { loc : { $within : { $polygon : { a : [-10, -10], b : [-10, 10], c : [10, 10], d : [10, -10] } } } } ).count() )

// Look in a box much bigger than the one we have data in
boxBounds = [[-100,-100], [-100, 100], [100,100], [100,-100]];
assert.eq( num , t.find( { loc : { "$within" : { "$polygon" : boxBounds } } } ).count() , "Big Bounding Box Test" );

t.drop();

pacman = [
           [0,2], [0,4], [2,6], [4,6], // Head
           [6,4], [4,3], [6,2], // Mouth
           [4,0], [2,0] // Bottom
         ];

t.save({loc: [1,3] }); // Add a point that's in
assert.isnull( db.getLastError() )

assert.eq( 1 , t.find({loc : { $within : { $polygon : pacman }}} ).count() , "Pacman single point" );

t.save({ loc : [5, 3] })  // Add a point that's out right in the mouth opening
t.save({ loc : [3, 7] })  // Add a point above the center of the head
t.save({ loc : [3,-1] })  // Add a point below the center of the bottom

assert.eq( 1 , t.find({loc : { $within : { $polygon : pacman }}} ).count() , "Pacman double point" );
