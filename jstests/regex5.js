
t = db.regex5
t.drop()

t.save( { x : [ "abc" , "xyz1" ] } )
t.save( { x : [ "ac" , "xyz2" ] } )

a = /.*b.*c/
x = /.*y.*/

doit = function() {
    
assert.eq( 1 , t.find( { x : a } ).count() , "A" )
assert.eq( 2 , t.find( { x : x } ).count() , "B" )
assert.eq( 2 , t.find( { x : { $in: [ x ] } } ).count() , "C" ) // SERVER-322
assert.eq( 1 , t.find( { x : { $in: [ a, "xyz1" ] } } ).count() , "D" ) // SERVER-322
assert.eq( 2 , t.find( { x : { $in: [ a, "xyz2" ] } } ).count() , "E" ) // SERVER-322
assert.eq( 1 , t.find( { x : { $all : [ a , x ] } } ).count() , "F" ) // SERVER-505
assert.eq( 1 , t.find( { x : { $all : [ a , "abc" ] } } ).count() , "G" ) // SERVER-505
assert.eq( 0 , t.find( { x : { $all : [ a , "ac" ] } } ).count() , "H" ) // SERVER-505
    
}

doit();
t.ensureIndex( {x:1} );
print( "now indexed" );
doit();

// check bound unions SERVER-322
assert.eq( [
            [ {x:1},{x:1} ],
            [ {x:2.5},{x:2.5} ],
            [ {x:"a"},{x:"a"} ],
            [ {x:"b"},{x:"e"} ],
            [ {x:/^b/},{x:/^b/} ],
            [ {x:/^c/},{x:/^c/} ],            
            [ {x:/^d/},{x:/^d/} ]
            ],
            t.find( { x : { $in: [ 1, 2.5, "a", "b", /^b/, /^c/, /^d/ ] } } ).explain().indexBounds );

// SERVER-505
assert.eq( [ [ {x:"a"}, {x:"a"} ] ], t.find( { x : { $all: [ "a", /^a/ ] } } ).explain().indexBounds );
assert.eq( [ [ {x:"a"}, {x:"b"} ] ], t.find( { x : { $all: [ /^a/ ] } } ).explain().indexBounds );
