// Make sure we only make a DBRef object for objects where the first field is a string named $ref
// and the second field is $id with any type. Only the first two fields matter for deciding if it
// is a DBRef. See http://docs.mongodb.org/manual/reference/database-references/#dbrefs.

var t = db.dbref3

// true cases
t.insert({sub: {$ref: "foo", $id: "bar"}, dbref: true});
t.insert({sub: {$ref: "foo", $id: "bar", $db: "baz"}, dbref: true});
t.insert({sub: {$ref: "foo", $id: "bar", db: "baz"}, dbref: true}); // out of spec but accepted
t.insert({sub: {$ref: "foo", $id: ObjectId()}, dbref: true});
t.insert({sub: {$ref: "foo", $id: 1}, dbref: true});

t.insert({sub: {$ref: 123/*not a string*/, $id: "bar"}, dbref: false});
t.insert({sub: {$id: "bar", $ref: "foo"}, dbref: false});
t.insert({sub: {$ref: "foo"}, dbref: false});
t.insert({sub: {$id: "foo"}, dbref: false});
t.insert({sub: {other: 1, $ref: "foo", $id: "bar"}, dbref: false});

t.find().forEach(function(obj) {
    assert.eq(obj.sub.constructor == DBRef, obj.dbref, tojson(obj));
});
