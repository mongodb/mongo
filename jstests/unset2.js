t = db.unset2;
t.drop();

t.save( {a:["a","b","c","d"]} );
t.update( {}, {$unset:{"a.3":1}} );
assert.eq( ["a","b","c"], t.findOne().a );
t.update( {}, {$unset:{"a.1":1}} );
assert.eq( ["a","c"], t.findOne().a );
t.update( {}, {$unset:{"a.0":1}} );
assert.eq( ["c"], t.findOne().a );