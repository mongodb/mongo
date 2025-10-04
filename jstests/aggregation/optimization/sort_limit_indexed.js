// server-7900 - $sort + $limit ignores limit when using index for sort

let c = db[jsTestName()];
c.drop();

for (let i = 0; i < 5; i++) c.insert({_id: i});

let res = c.aggregate({$sort: {_id: -1}}, {$limit: 2}); // uses index for sort
assert.eq(res.toArray(), [{_id: 4}, {_id: 3}]);
