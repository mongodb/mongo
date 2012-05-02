// use aggdb
db = db.getSiblingDB("aggdb");

// original crash from ticket
var r3 = db.runCommand({ aggregate:"article", pipeline:[
    { $project: {
        author: 1,
        _id: 0
    }},
    { $project: {
        Writer: "$author"
    }}
]});

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

assert(arrayEq(r3.result, r3result), 's5012 failed');
