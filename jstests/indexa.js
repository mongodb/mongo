
t = db.indexa;
t.drop();

t.ensureIndex( { x:1 }, true );

t.insert( { 'x':'A' } );
t.insert( { 'x':'B' } );
t.insert( { 'x':'A' } );

assert.eq( 2 , t.count() , "A" );

t.update( {x:'B'}, { x:'A' } );

a = t.find().toArray();
u = a.map( function(z){ return z.x } ).unique();

print("test commented out in indexa.js...");
//assert( a.length == u.length , "unique index update is broken" );

