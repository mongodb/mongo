
t = db.capped1;
t.drop();

db.createCollection("capped1" , {capped:true, size:1024 }); 
v = t.validate();
assert( v.valid , "A : " + tojson( v ) ); // SERVER-485

t.save( { x : 1 } )
assert( t.validate().valid , "B" )

