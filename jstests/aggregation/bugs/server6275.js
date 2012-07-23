// confirm that undefined no longer counts as 0 in $avg
c = db.c;
c.drop();
c.save({a:1});
c.save({a:4});
c.save({b:1});
assert.eq(c.aggregate({$group:{_id: null, avg:{$avg:"$a"}}}).result[0].avg, 2.5);

// again ensuring numberLongs work properly
c.drop();
c.save({a:NumberLong(1)});
c.save({a:NumberLong(4)});
c.save({b:NumberLong(1)});
assert.eq(c.aggregate({$group:{_id: null, avg:{$avg:"$a"}}}).result[0].avg, 2.5);
