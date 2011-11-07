// SERVER-4218
t = db.updatej;
t.drop();
t.ensureIndex( {age: 1} );
t.insert( { age: 5, name : "xxx" } );
t.insert( { age: 5, name : "yyy" } );
t.insert( { age: 5, name : "zzz" } );
t.ensureIndex( {age: 1} );
rec = t.findOne( {name: "yyy"} );
delete rec.name;
rec.age = NumberLong(5);
t.update( {name: "yyy"}, rec );
assert.eq( { age: NumberLong(5) }, t.find( {age:5}, {_id:0, age:1}).toArray()[1] );
