// https://jira.mongodb.org/browse/SERVER-5369
// correctly sort dates before 1970

// use the aggregation test db
db = db.getSiblingDB('aggdb');

db.test.drop();
db.test.insert({d:ISODate('1950-01-01')})
db.test.insert({d:ISODate('1980-01-01')})

var out = db.test.aggregate({$sort:{d:1}});

assert.lt(out.result[0].d, out.result[1].d)
