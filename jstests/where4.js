
db.where4.drop();

db.system.js.insert( { _id : "w4" , value : "5" } )

db.where4.insert( { x : 1 , y : 1 } )
db.where4.insert( { x : 2 , y : 1 } )

db.where4.update( { $where : function() { return this.x == 1; } } , 
                  { $inc : { y : 1 } } , false , true );


assert.eq( 2 , db.where4.findOne( { x : 1 } ).y )
assert.eq( 1 , db.where4.findOne( { x : 2 } ).y )

db.system.js.remove( { _id : "w4" } )
