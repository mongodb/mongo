// test $size
load('jstests/aggregation/extras/utils.js');

c = db.server4899;
c.drop();
c.save({arr:[]});
c.save({arr:[1]});
c.save({arr:["asdf", "asdfasdf"]});
c.save({arr:[1, "asdf", 1234, 4.3, {key:23}]});
c.save({arr:[3, [31, 31, 13, 13]]});

result = c.aggregate({$project: {_id: 0, length: {$size: "$arr"}}});
assert.eq(result.result, [{length:0},
                          {length:1},
                          {length:2},
                          {length:5},
                          {length:2}]);

c.save({arr:231});
assertErrorCode(c, {$project: {_id: 0, length: {$size: "$arr"}}}, 17124);
