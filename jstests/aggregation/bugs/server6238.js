// do not allow creation of fields with a $ prefix
load('jstests/aggregation/extras/utils.js');

c = db.c;
c.drop();

c.insert({a:1});

// assert that we get the proper error in both $project and $group
assertErrorCode(c, {$project:{$a:"$a"}}, 16404);
assertErrorCode(c, {$project:{a:{$b: "$a"}}}, 15999);
assertErrorCode(c, {$project:{a:{"$b": "$a"}}}, 15999);
assertErrorCode(c, {$project:{'a.$b':"$a"}}, 16410);
assertErrorCode(c, {$group:{_id: "$_id", $a:"$a"}}, 15950);
assertErrorCode(c, {$group:{_id: {$a:"$a"}}}, 15999);
