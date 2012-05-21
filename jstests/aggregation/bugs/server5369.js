// https://jira.mongodb.org/browse/SERVER-5369

// load the testing utilities
// load('utils.js')
// this is loaded by testbugs.js which calls us

// use the aggregation test db
db = db.getSiblingDB('aggdb');

db.test.drop();
db.test.save({a:1,b:2})
db.test.save({a:3,b:4})

var f = db.test.aggregate({$project:{a:0}});
