t = db.unset2;
t.drop();

t.save( {a:["a","b","c","d"]} );
t.update( {}, {$unset:{"a.3":1}} );
assert.eq( ["a","b","c"], t.findOne().a );
t.update( {}, {$unset:{"a.1":1}} );
assert.eq( ["a","c"], t.findOne().a );
t.update( {}, {$unset:{"a.0":1}} );
assert.eq( ["c"], t.findOne().a );

t.drop();
t.save( {a:["a","b","c","d","e"]} );
t.update( {}, {$unset:{"a.2":1},$set:{"a.3":3,"a.4":4,"a.5":5}} );
assert.eq( ["a","b",3,4,5], t.findOne().a );