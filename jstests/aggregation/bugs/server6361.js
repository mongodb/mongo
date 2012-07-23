// SERVER-6361 Disallow exclusions in $project for 2.2

c = db.c;
c.drop();

c.insert({a:2, nested: {_id:2, other:2}})
res = c.aggregate({$project: {a:0}})
assert.eq(res.code, 16406);

// excluding top-level _id is still allowed
res = c.aggregate({$project: {_id:0, a:1}})
assert.eq(res.result[0], {a:2});

// excluding nested _id is not
res = c.aggregate({$project: {'nested._id':0}})
assert.eq(res.code, 16406);

// nested _id is not automatically included
res = c.aggregate({$project: {_id:0, 'nested.other':1}})
assert.eq(res.result[0], {nested: {other:2}});

// not including anything is an error
res = c.aggregate({$project: {}})
assert.eq(res.code, 16403);

// even if you exclude _id
res = c.aggregate({$project: {'_id':0}})
assert.eq(res.code, 16403);
