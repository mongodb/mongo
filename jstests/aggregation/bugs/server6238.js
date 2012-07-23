// do not allow creation of fields with a $ prefix
c = db.c;
c.drop();

c.insert({a:1});

// assert that we get the proper error in both $project and $group
assert.eq(c.aggregate({$project:{$a:"$a"}}).code, 16404);
assert.eq(c.aggregate({$project:{a:{$b: "$a"}}}).code, 15999);
assert.eq(c.aggregate({$project:{a:{"$b": "$a"}}}).code, 15999);
assert.eq(c.aggregate({$project:{'a.$b':"$a"}}).code, 16410);
assert.eq(c.aggregate({$group:{_id: "$_id", $a:"$a"}}).code, 15950);
assert.eq(c.aggregate({$group:{_id: {$a:"$a"}}}).code, 15999);
