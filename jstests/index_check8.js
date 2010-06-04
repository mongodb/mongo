
t = db.index_check8
t.drop();

t.insert( { a : 1 , b : 1 , c : 1 , d : 1 , e : 1 } )
t.ensureIndex( { a : 1 , b : 1 , c : 1 } )
t.ensureIndex( { a : 1 , b : 1 , d : 1  , e : 1 } )

x = t.find( { a : 1 , b : 1 , d : 1 } ).sort( { e : 1 } ).explain()
assert( ! x.scanAndOrder , "A : " + tojson( x ) )

x = t.find( { a : 1 , b : 1 , c : 1 , d : 1 } ).sort( { e : 1 } ).explain()
//assert( ! x.scanAndOrder , "B : " + tojson( x ) )


