
t = db.update_arraymatch4
t.drop()

x = { _id : 1 , cast : ["John","Hugh","Halle"] , movie:"Swordfish" }
t.insert( x )
assert.eq( x , t.findOne() , "A1" )

x.cast[0] = "John Travolta"
t.update( { cast : "John" } , { $set : { "cast.$" : "John Travolta" } } )
assert.eq( x , t.findOne() , "A2" )

t.ensureIndex( { cast : 1 } )
x.cast[0] = "xxx"
t.update( { cast : "John Travolta" } , { $set : { "cast.$" : "xxx" } } )
//assert.eq( x , t.findOne() , "A3" ); // SERVER-1055


