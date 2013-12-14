
t = db.array4;
t.drop();

t.insert({"a": ["1", "2", "3"]});
t.insert({"a" : ["2", "1"]});

var x = {'a.0' : /1/};

assert.eq(t.count(x), 1);

assert.eq(t.findOne(x).a[0], 1);
assert.eq(t.findOne(x).a[1], 2);

t.drop();

t.insert({"a" : {"0" : "1"}});
t.insert({"a" : ["2", "1"]});

assert.eq(t.count(x), 1);
assert.eq(t.findOne(x).a[0], 1);

t.drop();

t.insert({"a" : ["0", "1", "2", "3", "4", "5", "6", "1", "1", "1", "2", "3", "2", "1"]});
t.insert({"a" : ["2", "1"]});

x = {"a.12" : /2/};
assert.eq(t.count(x), 1);
assert.eq(t.findOne(x).a[0], 0);

// A query for an undefined value should not match an array.
t.drop();
t.insert( { a:[ 5 ] } );
t.insert( { a:[ 5, 6 ] } );
t.insert( { a:[ 5, "foo" ] } );
assert.eq( 0, t.count( { a:{ $in:[ undefined ] } } ) );
