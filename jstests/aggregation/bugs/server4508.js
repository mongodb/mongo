// https://jira.mongodb.org/browse/SERVER-4508

// use the aggregation test db
db = db.getSiblingDB('aggdb');

db.c.drop();
db.c.save({_id:1, a:1, b:2, c:[], d:4});

var a1 = db.runCommand({ aggregate:"c", pipeline:[
    {$unwind: "$c"}
]});

var a1result = [
    {
        "_id" : 1,
        "a" : 1,
        "b" : 2,
        "d" : 4
    }
];

assert.eq(a1.result, a1result, 'server4508 failed');
