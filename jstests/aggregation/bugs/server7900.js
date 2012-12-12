// server-7900 - $sort + $limit ignores limit when using index for sort

c = db.s7900;
c.drop();

for (var i=0; i < 5; i++)
    c.insert({_id:i});

res = c.aggregate({$sort: {_id: -1}}, {$limit: 2}); // uses index for sort
assert.eq(res.result, [{_id: 4}, {_id: 3}]);

