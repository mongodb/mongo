// https://jira.mongodb.org/browse/SERVER-4508

// load the testing utilities
// load('utils.js')
// this is loaded by testbugs.js which calls us

// use the aggregation test db
db = db.getSiblingDB('aggdb');

db.c.drop();
db.c.save({a:1, b:2, c:[], d:4});

var a1 = db.runCommand({ aggregate:"c", pipeline:[
    {$unwind: "$c"}
]});

var a1result = [
    {
        "_id" : ObjectId("4efe0a07ef25a330e574a47f"),
        "a" : 1,
        "b" : 2,
        "d" : 4
    }
];

assert(arrayEq(a1.result, a1result), 'server4508 failed');
