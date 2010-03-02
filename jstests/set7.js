// test $set with array indicies

t = db.jstests_set7;

t.drop();

t.save( {a:[0,1,2,3]} );
t.update( {}, {$set:{"a.0":2}} );
assert.eq( [2,1,2,3], t.findOne().a );

t.update( {}, {$set:{"a.4":5}} );
assert.eq( [2,1,2,3,5], t.findOne().a );

t.update( {}, {$set:{"a.9":9}} );
assert.eq( [2,1,2,3,5,null,null,null,null,9], t.findOne().a );