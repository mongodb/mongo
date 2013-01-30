// use aggdb
db = db.getSiblingDB("aggdb");
var article = db.article;

load('jstests/aggregation/data/articles.js');

// original crash from ticket
var r3 = article.aggregate(
    { $project: {
        author: 1,
        _id: 0
    }},
    { $project: {
        Writer: "$author"
    }}
);

printjson(r3);

var r3result = [
    {
        "Writer" : "bob"
    },
    {
        "Writer" : "dave"
    },
    {
        "Writer" : "jane"
    }
];

assert.eq(r3.result, r3result, 's5012 failed');
