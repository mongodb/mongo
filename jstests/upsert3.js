// tests to make sure no dup fields are created when using query to do upsert
t = db.upsert1;
t.drop();

// make sure the new _id is not duplicated
t.update( {"a.b": 1, a: {a: 1, b: 1}} , {$inc: {y: 1}} , true );
assert.gleError(db, function(gle) {
    return "a.b-1 - " + tojson(gle) + " doc:" + tojson(t.findOne()) });

t.update( {"_id.a": 1, _id: {a: 1, b: 1}} , {$inc : {y: 1}} , true );
assert.gleError(db, function(gle) {
    return "_id-1 - " + tojson(gle) + " doc:" + tojson(t.findOne()) });

t.update( {_id: {a: 1, b: 1}, "_id.a": 1} , { $inc: {y: 1}} , true );
assert.gleError(db, function(gle) {
    return "_id-2 - " + tojson(gle) + " doc:" + tojson(t.findOne()) });

// Should be redundant, but including from SERVER-11363
t.update( {_id: {a: 1, b: 1}, "_id.a": 1} , {$setOnInsert: {y: 1}} , true );
assert.gleError(db, function(gle) {
    return "_id-3 - " + tojson(gle) + " doc:" + tojson(t.findOne()) });

//Should be redundant, but including from SERVER-11514
t.update( {"a": {}, "a.c": 2} , {$set: {x: 1}}, true );
assert.gleError(db, function(gle) {
    return "a.c-1 - " + tojson(gle) + " doc:" + tojson(t.findOne()) });

// Should be redundant, but including from SERVER-4830
t.update( {'a': {b: 1}, 'a.c': 1}, {$inc: {z: 1}}, true );
assert.gleError(db, function(gle) {
    return "a-1 - " + tojson(gle) + " doc:" + tojson(t.findOne()) });

// Should be redundant, but including from SERVER-4830
t.update( {a: 1, "a.b": 1, a: [1, {b: 1}]}, {$inc: {z: 1}}, true );
assert.gleError(db, function(gle) {
    return "a-2 - " + tojson(gle) + " doc:" + tojson(t.findOne()) });

// Replacement tests
// Query is ignored for replacements, except _id field.
t.update( {r: {a: 1, b: 1}, "r.a": 1} , {y: 1} , true );
assert.gleSuccess(db, "r-1");
assert(t.findOne().y, 1, "inserted doc missing field")
var docMinusId = t.findOne();
delete docMinusId._id
assert.docEq({y: 1}, docMinusId, "r-1")
t.drop()

t.update( {_id: {a:1, b:1}, "_id.a": 1} , {y: 1} , true );
assert.gleSuccess(db, "_id-4");
assert.docEq({_id: {a: 1, b: 1}, y: 1}, t.findOne(), "_id-4")
t.drop()