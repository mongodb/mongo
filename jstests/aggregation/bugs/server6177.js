// SERVER-6177: better error when projecting into a subfield with an existing expression
c = db.c;
c.drop();

c.save( {} );

res = c.aggregate( { $project:{ 'x':{ $add:[ 1 ] }, 'x.b':1 } } );
assert.eq(res.code, 16401);

// These currently give different errors
res = c.aggregate( { $project:{ 'x.b': 1, 'x':{ $add:[ 1 ] }} } );
assert.eq(res.code, 16400);

// These both give the same error however
res = c.aggregate( { $project:{'x':{'b':1}, 'x.b': 1} } );
assert.eq(res.code, 16400);

res = c.aggregate( { $project:{'x.b': 1, 'x':{'b':1}} } );
assert.eq(res.code, 16400);
