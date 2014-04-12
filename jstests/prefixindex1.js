var t = db.prefixindex1;
t.drop()

// only single field prefix index is supported.
var badspec = {a : "prefix", b : 1};
t.ensureIndex( badspec );
assert.eq( t.getIndexes().length, 1, "prefix indexes on multiple fields is not supported");

var goodspec = {a : "prefix"};
// prefix indexes cannot be unique.
t.ensureIndex( goodspec , {"unique" : true});
assert.eq( t.getIndexes().length , 1, "prefix index got created with unique property.");

t.insert( {b : 1});
t.insert( {b : 2});

t.ensureIndex( {b : "prefix"} );
assert.eq( t.getIndexes().length, 1, "prefix index got created for number field");

// check the syntax of prefix index creation.
t.ensureIndex( goodspec );
assert.eq( t.getIndexes().length , 2, "prefix index didn't get created");

t.dropIndex( goodspec );
t.drop();

// check the syntax of prefix index creation when prefixLength is specified.
t.ensureIndex( goodspec , {"prefixLength" : 8});
assert.eq( t.getIndexes().length, 2, "prefix index didn't get created");


//test basic inserts
t.insert( {a : "testing prefix indexes"} );
t.insert( {a : "prefix length is 8"} );
t.insert( {a : "inserting third document"} );
// insert a field with length less than the prefixLength
t.insert( {a : "len<8"} );
t.insert( {a : "make the total count 5"} );

// check count works
assert.eq( t.find().count(), 5, "basic count() didn't work");

// insert null value for field "a"
t.insert( {b : 1});
t.insert( {b : 2});

// verify querying works
assert.eq( t.find( {a : "prefix length is 8"} ).toArray().length, 1);
assert.eq( t.find( {a : "len<8"} ).toArray().length, 1);

//test the prefix cursor is getting used in queries
var cursorname = "BtreeCursor a_prefix";
assert.eq( t.find({a : "prefix length is 8"}).explain().cursor,
           cursorname, "query is not using prefix cursor");
assert.eq( t.find({a : "len<8"}).explain().cursor,
           cursorname, "query is not using prefix cursor");
assert.eq( t.find({a : {$lte : "len<8"}}).explain().cursor,
           cursorname, "query is not using prefix cursor");
assert.eq( t.find({a : {$lt : "len<8"}}).explain().cursor,
           cursorname, "query is not using prefix cursor");
assert.eq( t.find({a : {$gt : "len<8"}}).explain().cursor,
           cursorname, "query is not using prefix cursor");
assert.eq( t.find({a : {$gte : "len<8"}}).explain().cursor,
           cursorname, "query is not using prefix cursor");

// test prefix cursor is getting used in prefix regex search
assert.eq( t.find({ a : /^prefix .*/ }).explain().cursor,
           cursorname, "prefix regex search is not using perfix cursor");

// test prefix cursor is getting with IN operator
assert.eq( t.find({ a : {$in : ["in operator", "len<8"] } }).explain().cursor,
           cursorname, "prefix cursor is not getting used with IN operator");

t.drop();
t.dropIndex( goodspec );


// checking prefix index on a field with binary data
t.insert( {a : new BinData(0, "k4wuHUffrgwe3jF05MCY") });
t.insert( {a : new BinData(0, "TWFuIGlzIGRpc3Rpbmd1aXNo") });

t.ensureIndex( goodspec , {"prefixLength" : 10});

// check count works
assert.eq( t.find().count(), 2, "basic count() didn't work");
assert.eq( t.find({a : new BinData(0, "k4wuHUffrgwe3jF05MCY")}).explain().cursor,
           cursorname, "query is not using prefix cursor");

assert.eq( t.find({a : new BinData(0, "k4wuHUffrgwe3jF05MCY")}).toArray().length, 1);
