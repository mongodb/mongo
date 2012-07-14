// SERVER-6181 Correctly support an expression for _id

c = db.c;
c.drop();

c.save( { a:2 } );

res = c.aggregate( { $project:{ _id:'$a' } } )
assert.eq(res.result, [{_id:2}])

res = c.aggregate( { $project:{ _id:{$add: [1, '$a']} } } )
assert.eq(res.result, [{_id:3}])
