// https://jira.mongodb.org/browse/SERVER-5369
// correctly sort dates before 1970

// use the aggregation test db
const testDb = db.getSiblingDB("aggdb");

testDb.test.drop();
testDb.test.insert({d: ISODate("1950-01-01")});
testDb.test.insert({d: ISODate("1980-01-01")});

let out = testDb.test.aggregate({$sort: {d: 1}}).toArray();

assert.lt(out[0].d, out[1].d);
