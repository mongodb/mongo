
t = db.regex5
t.drop()

t.save( { x : [ "abc" , "xyz" ] } )
t.save( { x : [ "ac" , "xyz" ] } )

a = /.*b.*c/
x = /.*y.*/

assert.eq( 1 , t.find( { x : a } ).count() , "A" )
assert.eq( 2 , t.find( { x : x } ).count() , "B" )
// assert.eq( 1 , t.find( { x : { $all : [ a , x ] } } ).count() , "C" ) // SERVER-505
