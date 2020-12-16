// Make sure we only make a DBRef object for objects where the first field is a string named $ref
// and the second field is $id with any type. Only the first two fields matter for deciding if it
// is a DBRef. See http://docs.mongodb.org/manual/reference/database-references/#dbrefs.

var t = db.dbref3;

t.drop();

// true cases
t.insert({sub: {$ref: "foo", $id: "bar"}, dbref: true});
t.insert({sub: {$ref: "foo", $id: "bar", $db: "baz"}, dbref: true});
t.insert({sub: {$ref: "foo", $id: "bar", db: "baz"}, dbref: true});  // out of spec but accepted
t.insert({sub: {$ref: "foo", $id: ObjectId()}, dbref: true});
t.insert({sub: {$ref: "foo", $id: 1}, dbref: true});

t.insert({sub: {$ref: 123 /*not a string*/, $id: "bar"}, dbref: false});
t.insert({sub: {$id: "bar", $ref: "foo"}, dbref: false});
t.insert({sub: {$ref: "foo"}, dbref: false});
t.insert({sub: {$id: "foo"}, dbref: false});
t.insert({sub: {other: 1, $ref: "foo", $id: "bar"}, dbref: false});

t.find().forEach(function(obj) {
    assert.eq(obj.sub.constructor == DBRef, obj.dbref, tojson(obj));
});

// We should be able to run distinct against DBRef fields.
var distinctRefs = t.distinct('sub.$ref');
print('distinct $ref = ' + distinctRefs);

var distinctIDs = t.distinct('sub.$id');
print('distinct $id = ' + distinctIDs);

var distinctDBs = t.distinct('sub.$db');
print('distinct $db = ' + distinctDBs);

// Confirm number of unique values in each DBRef field.
assert.eq(2, distinctRefs.length);
assert.eq(4, distinctIDs.length);
assert.eq(1, distinctDBs.length);

// $id is an array. perform positional projection on $id.
t.insert({sub: {$ref: "foo", $id: [{x: 1, y: 1}, {x: 2, y: 2}, {x: 3, y: 3}]}});
var k = t.findOne({'sub.$id': {$elemMatch: {x: 2}}}, {_id: 0, 'sub.$id.$': 1});
print('k = ' + tojson(k));
assert.eq({sub: {$id: [{x: 2, y: 2}]}}, k);

// Check that DBRef fields can be excluded
assert.commandWorked(
    t.insert({_id: 0, shouldExclude: 1, sub: {$ref: "foo", $id: 10, $db: "someDb"}}));
assert.eq(t.find({shouldExclude: 1}, {"sub.$ref": 0}).toArray(),
          [{_id: 0, shouldExclude: 1, sub: {$id: 10, $db: "someDb"}}]);
assert.eq(t.find({shouldExclude: 1}, {"sub.$id": 0}).toArray(),
          [{_id: 0, shouldExclude: 1, sub: {$ref: "foo", $db: "someDb"}}]);
assert.eq(t.find({shouldExclude: 1}, {"sub.$id": 0, "sub.$ref": 0, "sub.$db": 0}).toArray(),
          [{_id: 0, shouldExclude: 1, sub: {}}]);

// It should be legal to exclude a DBRef field anywhere, even at the top layer.
assert.eq(t.aggregate([{$match: {shouldExclude: 1}}, {$project: {"$id": 0, sub: 0}}]).toArray(),
          [{_id: 0, shouldExclude: 1}]);
