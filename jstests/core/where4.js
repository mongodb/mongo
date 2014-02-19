
db.where4.drop();

db.system.js.insert( { _id : "w4" , value : "5" } )

db.where4.insert( { x : 1 , y : 1 } )
db.where4.insert( { x : 2 , y : 1 } )

db.where4.update( { $where : function() { return this.x == 1; } } , 
                  { $inc : { y : 1 } } , false , true );


assert.eq( 2 , db.where4.findOne( { x : 1 } ).y )
assert.eq( 1 , db.where4.findOne( { x : 2 } ).y )

// Test that where queries work with stored javascript
db.system.js.save( { _id : "where4_addOne" , value : function(x) { return x + 1; } } )

db.where4.update( { $where : "where4_addOne(this.x) == 2" } ,
                  { $inc : { y : 1 } } , false , true );

assert.eq( 3 , db.where4.findOne( { x : 1 } ).y )
assert.eq( 1 , db.where4.findOne( { x : 2 } ).y )

db.system.js.remove( { _id : "where4_equalsOne" } )

db.system.js.remove( { _id : "w4" } )
