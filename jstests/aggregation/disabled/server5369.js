// https://jira.mongodb.org/browse/SERVER-5369
// make sure excluding a field doesnt cause _id to appear twice

// use the aggregation test db
db = db.getSiblingDB('aggdb');

// empty and populate
db.test.drop();
db.test.save({a:1,b:2})

// agg with exclusion than ensure fields are only the two we expect
var f = db.test.aggregate({$project:{a:0}});
assert.eq(["_id","b"], Object.keySet(f.result[0]), "server5369 failed");
