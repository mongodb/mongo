(function() {
"use strict";

// Loads data into the namespace 'aggdb.articles'.
load('jstests/aggregation/data/articles.js');
load('jstests/aggregation/extras/utils.js');

const testDB = db.getSiblingDB("aggdb");

// just passing through fields
let p1 = testDB.runCommand({
    aggregate: "article",
    pipeline: [{$project: {tags: 1, pageViews: 1}}, {$sort: {_id: 1}}],
    cursor: {}
});

let p1result = [
    {"_id": 1, "pageViews": 5, "tags": ["fun", "good", "fun"]},
    {"_id": 2, "pageViews": 7, "tags": ["fun", "nasty"]},
    {"_id": 3, "pageViews": 6, "tags": ["nasty", "filthy"]}
];

assert.docEq(p1.cursor.firstBatch, p1result, 'p1 failed');

// a simple array unwinding
let u1 = testDB.runCommand({aggregate: "article", pipeline: [{$unwind: "$tags"}], cursor: {}});

let u1result = [
    {
        "_id": 1,
        "title": "this is my title",
        "author": "bob",
        "posted": ISODate("2004-03-21T18:59:54Z"),
        "pageViews": 5,
        "tags": "fun",
        "comments":
            [{"author": "joe", "text": "this is cool"}, {"author": "sam", "text": "this is bad"}],
        "other": {"foo": 5}
    },
    {
        "_id": 1,
        "title": "this is my title",
        "author": "bob",
        "posted": ISODate("2004-03-21T18:59:54Z"),
        "pageViews": 5,
        "tags": "good",
        "comments":
            [{"author": "joe", "text": "this is cool"}, {"author": "sam", "text": "this is bad"}],
        "other": {"foo": 5}
    },
    {
        "_id": 1,
        "title": "this is my title",
        "author": "bob",
        "posted": ISODate("2004-03-21T18:59:54Z"),
        "pageViews": 5,
        "tags": "fun",
        "comments":
            [{"author": "joe", "text": "this is cool"}, {"author": "sam", "text": "this is bad"}],
        "other": {"foo": 5}
    },
    {
        "_id": 2,
        "title": "this is your title",
        "author": "dave",
        "posted": ISODate("2030-08-08T04:11:10Z"),
        "pageViews": 7,
        "tags": "fun",
        "comments": [
            {"author": "barbara", "text": "this is interesting"},
            {"author": "jenny", "text": "i like to play pinball", "votes": 10}
        ],
        "other": {"bar": 14}
    },
    {
        "_id": 2,
        "title": "this is your title",
        "author": "dave",
        "posted": ISODate("2030-08-08T04:11:10Z"),
        "pageViews": 7,
        "tags": "nasty",
        "comments": [
            {"author": "barbara", "text": "this is interesting"},
            {"author": "jenny", "text": "i like to play pinball", "votes": 10}
        ],
        "other": {"bar": 14}
    },
    {
        "_id": 3,
        "title": "this is some other title",
        "author": "jane",
        "posted": ISODate("2000-12-31T05:17:14Z"),
        "pageViews": 6,
        "tags": "nasty",
        "comments": [
            {"author": "will", "text": "i don't like the color"},
            {"author": "jenny", "text": "can i get that in green?"}
        ],
        "other": {"bar": 14}
    },
    {
        "_id": 3,
        "title": "this is some other title",
        "author": "jane",
        "posted": ISODate("2000-12-31T05:17:14Z"),
        "pageViews": 6,
        "tags": "filthy",
        "comments": [
            {"author": "will", "text": "i don't like the color"},
            {"author": "jenny", "text": "can i get that in green?"}
        ],
        "other": {"bar": 14}
    }
];

let firstBatch = u1.cursor.firstBatch;
assert(arrayEq(firstBatch, u1result), tojson({got: firstBatch, expected: u1result}));

// unwind an array at the end of a dotted path
testDB.ut.drop();
assert.writeOK(testDB.ut.insert({_id: 4, a: 1, b: {e: 7, f: [4, 3, 2, 1]}, c: 12, d: 17}));
let u2 = testDB.runCommand(
    {aggregate: "ut", pipeline: [{$unwind: "$b.f"}, {$sort: {"b.f": -1}}], cursor: {}});

let u2result = [
    {"_id": 4, "a": 1, "b": {"e": 7, "f": 4}, "c": 12, "d": 17},
    {"_id": 4, "a": 1, "b": {"e": 7, "f": 3}, "c": 12, "d": 17},
    {"_id": 4, "a": 1, "b": {"e": 7, "f": 2}, "c": 12, "d": 17},
    {"_id": 4, "a": 1, "b": {"e": 7, "f": 1}, "c": 12, "d": 17}
];

assert.docEq(u2.cursor.firstBatch, u2result, 'u2 failed');

// combining a projection with unwinding an array
let p2 = testDB.runCommand({
    aggregate: "article",
    pipeline: [{$project: {author: 1, tags: 1, pageViews: 1}}, {$unwind: "$tags"}],
    cursor: {}
});

let p2result = [
    {"_id": 1, "author": "bob", "pageViews": 5, "tags": "fun"},
    {"_id": 1, "author": "bob", "pageViews": 5, "tags": "good"},
    {"_id": 1, "author": "bob", "pageViews": 5, "tags": "fun"},
    {"_id": 2, "author": "dave", "pageViews": 7, "tags": "fun"},
    {"_id": 2, "author": "dave", "pageViews": 7, "tags": "nasty"},
    {"_id": 3, "author": "jane", "pageViews": 6, "tags": "nasty"},
    {"_id": 3, "author": "jane", "pageViews": 6, "tags": "filthy"}
];

firstBatch = p2.cursor.firstBatch;
assert(arrayEq(firstBatch, p2result), tojson({got: firstBatch, expected: p2result}));

// pulling values out of subdocuments
let p3 = testDB.runCommand({
    aggregate: "article",
    pipeline: [{$project: {otherfoo: "$other.foo", otherbar: "$other.bar"}}, {$sort: {_id: 1}}],
    cursor: {}
});

let p3result = [{"_id": 1, "otherfoo": 5}, {"_id": 2, "otherbar": 14}, {"_id": 3, "otherbar": 14}];

assert.docEq(p3.cursor.firstBatch, p3result, 'p3 failed');

// projection includes a computed value
let p4 = testDB.runCommand({
    aggregate: "article",
    pipeline: [{$project: {author: 1, daveWroteIt: {$eq: ["$author", "dave"]}}}, {$sort: {_id: 1}}],
    cursor: {}
});

let p4result = [
    {"_id": 1, "author": "bob", "daveWroteIt": false},
    {"_id": 2, "author": "dave", "daveWroteIt": true},
    {"_id": 3, "author": "jane", "daveWroteIt": false}
];

assert.docEq(p4.cursor.firstBatch, p4result, 'p4 failed');

// projection includes a virtual (fabricated) document
let p5 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {$project: {author: 1, pageViews: 1, tags: 1}},
        {$unwind: "$tags"},
        {$project: {author: 1, subDocument: {foo: "$pageViews", bar: "$tags"}}}
    ],
    cursor: {}
});

let p5result = [
    {"_id": 1, "author": "bob", "subDocument": {"foo": 5, "bar": "fun"}},
    {"_id": 1, "author": "bob", "subDocument": {"foo": 5, "bar": "good"}},
    {"_id": 1, "author": "bob", "subDocument": {"foo": 5, "bar": "fun"}},
    {"_id": 2, "author": "dave", "subDocument": {"foo": 7, "bar": "fun"}},
    {"_id": 2, "author": "dave", "subDocument": {"foo": 7, "bar": "nasty"}},
    {"_id": 3, "author": "jane", "subDocument": {"foo": 6, "bar": "nasty"}},
    {"_id": 3, "author": "jane", "subDocument": {"foo": 6, "bar": "filthy"}}
];

firstBatch = p5.cursor.firstBatch;
assert(arrayEq(firstBatch, p5result), tojson({got: firstBatch, expected: p5result}));

// multi-step aggregate
// nested expressions in computed fields
let p6 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {$project: {author: 1, tags: 1, pageViews: 1}},
        {$unwind: "$tags"},
        {
            $project: {
                author: 1,
                tag: "$tags",
                pageViews: 1,
                daveWroteIt: {$eq: ["$author", "dave"]},
                weLikeIt: {$or: [{$eq: ["$author", "dave"]}, {$eq: ["$tags", "good"]}]}
            }
        }
    ],
    cursor: {}
});

let p6result = [
    {
        "_id": 1,
        "author": "bob",
        "pageViews": 5,
        "tag": "fun",
        "daveWroteIt": false,
        "weLikeIt": false
    },
    {
        "_id": 1,
        "author": "bob",
        "pageViews": 5,
        "tag": "good",
        "daveWroteIt": false,
        "weLikeIt": true
    },
    {
        "_id": 1,
        "author": "bob",
        "pageViews": 5,
        "tag": "fun",
        "daveWroteIt": false,
        "weLikeIt": false
    },
    {
        "_id": 2,
        "author": "dave",
        "pageViews": 7,
        "tag": "fun",
        "daveWroteIt": true,
        "weLikeIt": true
    },
    {
        "_id": 2,
        "author": "dave",
        "pageViews": 7,
        "tag": "nasty",
        "daveWroteIt": true,
        "weLikeIt": true
    },
    {
        "_id": 3,
        "author": "jane",
        "pageViews": 6,
        "tag": "nasty",
        "daveWroteIt": false,
        "weLikeIt": false
    },
    {
        "_id": 3,
        "author": "jane",
        "pageViews": 6,
        "tag": "filthy",
        "daveWroteIt": false,
        "weLikeIt": false
    }
];

firstBatch = p6.cursor.firstBatch;
assert(arrayEq(firstBatch, p6result), tojson({got: firstBatch, expected: p6result}));

// slightly more complex computed expression; $ifNull
let p7 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {$project: {theSum: {$add: ["$pageViews", {$ifNull: ["$other.foo", "$other.bar"]}]}}},
        {$sort: {_id: 1}}
    ],
    cursor: {}
});

let p7result = [{"_id": 1, "theSum": 10}, {"_id": 2, "theSum": 21}, {"_id": 3, "theSum": 20}];

assert.docEq(p7.cursor.firstBatch, p7result, 'p7 failed');

// dotted path inclusion; _id exclusion
let p8 = testDB.runCommand({
    aggregate: "article",
    pipeline: [{$project: {_id: 0, author: 1, tags: 1, "comments.author": 1}}, {$unwind: "$tags"}],
    cursor: {}
});

let p8result = [
    {"author": "bob", "tags": "fun", "comments": [{"author": "joe"}, {"author": "sam"}]},
    {"author": "bob", "tags": "good", "comments": [{"author": "joe"}, {"author": "sam"}]},
    {"author": "bob", "tags": "fun", "comments": [{"author": "joe"}, {"author": "sam"}]},
    {"author": "dave", "tags": "fun", "comments": [{"author": "barbara"}, {"author": "jenny"}]},
    {"author": "dave", "tags": "nasty", "comments": [{"author": "barbara"}, {"author": "jenny"}]},
    {"author": "jane", "tags": "nasty", "comments": [{"author": "will"}, {"author": "jenny"}]},
    {"author": "jane", "tags": "filthy", "comments": [{"author": "will"}, {"author": "jenny"}]}
];

firstBatch = p8.cursor.firstBatch;
assert(arrayEq(firstBatch, p8result), tojson({got: firstBatch, expected: p8result}));

// collapse a dotted path with an intervening array
let p9 = testDB.runCommand({
    aggregate: "article",
    pipeline:
        [{$project: {_id: 0, author: 1, commentsAuthor: "$comments.author"}}, {$sort: {author: 1}}],
    cursor: {}
});

let p9result = [
    {"author": "bob", "commentsAuthor": ["joe", "sam"]},
    {"author": "dave", "commentsAuthor": ["barbara", "jenny"]},
    {"author": "jane", "commentsAuthor": ["will", "jenny"]}
];

assert.docEq(p9.cursor.firstBatch, p9result, 'p9 failed');

// simple sort
let p10 = testDB.runCommand({aggregate: "article", pipeline: [{$sort: {title: 1}}], cursor: {}});

let p10result = [
    {
        "_id": 1,
        "title": "this is my title",
        "author": "bob",
        "posted": ISODate("2004-03-21T18:59:54Z"),
        "pageViews": 5,
        "tags": ["fun", "good", "fun"],
        "comments":
            [{"author": "joe", "text": "this is cool"}, {"author": "sam", "text": "this is bad"}],
        "other": {"foo": 5}
    },
    {
        "_id": 3,
        "title": "this is some other title",
        "author": "jane",
        "posted": ISODate("2000-12-31T05:17:14Z"),
        "pageViews": 6,
        "tags": ["nasty", "filthy"],
        "comments": [
            {"author": "will", "text": "i don't like the color"},
            {"author": "jenny", "text": "can i get that in green?"}
        ],
        "other": {"bar": 14}
    },
    {
        "_id": 2,
        "title": "this is your title",
        "author": "dave",
        "posted": ISODate("2030-08-08T04:11:10Z"),
        "pageViews": 7,
        "tags": ["fun", "nasty"],
        "comments": [
            {"author": "barbara", "text": "this is interesting"},
            {"author": "jenny", "text": "i like to play pinball", "votes": 10}
        ],
        "other": {"bar": 14}
    }
];

assert.docEq(p10.cursor.firstBatch, p10result, 'p10 failed');

// unwind on nested array
testDB.p11.drop();
testDB.p11.save({
    _id: 5,
    name: 'MongoDB',
    items: {authors: ['jay', 'vivek', 'bjornar'], dbg: [17, 42]},
    favorites: ['pickles', 'ice cream', 'kettle chips']
});

let p11 = testDB.runCommand({
    aggregate: "p11",
    pipeline: [
        {$unwind: "$items.authors"},
        {$project: {name: 1, author: "$items.authors"}},
        {$sort: {author: 1}}

    ],
    cursor: {}
});

let p11result = [
    {"_id": 5, "name": "MongoDB", "author": "bjornar"},
    {"_id": 5, "name": "MongoDB", "author": "jay"},
    {"_id": 5, "name": "MongoDB", "author": "vivek"},
];

assert.docEq(p11.cursor.firstBatch, p11result, 'p11 failed');

// multiply test
let p12 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {
            $project:
                {theProduct: {$multiply: ["$pageViews", {$ifNull: ["$other.foo", "$other.bar"]}]}},
        },
        {$sort: {_id: 1}}
    ],
    cursor: {}
});

let p12result =
    [{"_id": 1, "theProduct": 25}, {"_id": 2, "theProduct": 98}, {"_id": 3, "theProduct": 84}];

assert.docEq(p12.cursor.firstBatch, p12result, 'p12 failed');

// subtraction test
let p13 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {
            $project: {
                theDifference:
                    {$subtract: ["$pageViews", {$ifNull: ["$other.foo", "$other.bar"]}]}
            }
        },
        {$sort: {_id: 1}}
    ],
    cursor: {}
});

let p13result = [
    {"_id": 1, "theDifference": 0},
    {"_id": 2, "theDifference": -7},
    {"_id": 3, "theDifference": -8}
];

assert.docEq(p13.cursor.firstBatch, p13result, 'p13 failed');

// mod test
let p14 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {
            $project: {
                theRemainder: {
                    $mod: [
                        {$ifNull: ["$other.foo", "$other.bar"]},
                        "$pageViews",
                    ]
                }
            }
        },
        {$sort: {_id: 1}}
    ],
    cursor: {}
});

let p14result =
    [{"_id": 1, "theRemainder": 0}, {"_id": 2, "theRemainder": 0}, {"_id": 3, "theRemainder": 2}];

assert.docEq(p14.cursor.firstBatch, p14result, 'p14 failed');

// toUpper test
let p15 = testDB.runCommand({
    aggregate: "article",
    pipeline: [{$project: {author: {$toUpper: "$author"}, pageViews: 1}}, {$sort: {_id: 1}}],
    cursor: {}
});

let p15result = [
    {"_id": 1, "author": "BOB", "pageViews": 5},
    {"_id": 2, "author": "DAVE", "pageViews": 7},
    {"_id": 3, "author": "JANE", "pageViews": 6}
];

assert.docEq(p15.cursor.firstBatch, p15result, 'p15 failed');

// toLower test
let p16 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {$project: {author: {$toUpper: "$author"}, pageViews: 1}},
        {$project: {author: {$toLower: "$author"}, pageViews: 1}},
        {$sort: {_id: 1}}
    ],
    cursor: {}
});

let p16result = [
    {
        "_id": 1,
        "author": "bob",
        "pageViews": 5,
    },
    {
        "_id": 2,
        "author": "dave",
        "pageViews": 7,
    },
    {
        "_id": 3,
        "author": "jane",
        "pageViews": 6,
    }
];

assert.docEq(p16.cursor.firstBatch, p16result, 'p16 failed');

// substr test
let p17 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {
            $project: {
                author: {$substrBytes: ["$author", 1, 2]},
            }
        },
        {$sort: {_id: 1}}
    ],
    cursor: {}
});

let p17result =
    [{"_id": 1, "author": "ob"}, {"_id": 2, "author": "av"}, {"_id": 3, "author": "an"}];

assert.docEq(p17.cursor.firstBatch, p17result, 'p17 failed');

// strcasecmp test
let p18 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {
            $project: {
                tags: 1,
                thisisalametest: {$strcasecmp: ["foo", "bar"]},
                thisisalamepass: {$strcasecmp: ["foo", "foo"]}
            }
        },
        {$sort: {_id: 1}}
    ],
    cursor: {}
});

let p18result = [
    {"_id": 1, "tags": ["fun", "good", "fun"], "thisisalametest": 1, "thisisalamepass": 0},
    {"_id": 2, "tags": ["fun", "nasty"], "thisisalametest": 1, "thisisalamepass": 0},
    {"_id": 3, "tags": ["nasty", "filthy"], "thisisalametest": 1, "thisisalamepass": 0}
];

assert.docEq(p18.cursor.firstBatch, p18result, 'p18 failed');

// date tests
let p19 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {
            $project: {
                authors: 1,
                posted: 1,
                seconds: {$second: "$posted"},
                minutes: {$minute: "$posted"},
                hour: {$hour: "$posted"},
                dayOfYear: {$dayOfYear: "$posted"},
                dayOfMonth: {$dayOfMonth: "$posted"},
                dayOfWeek: {$dayOfWeek: "$posted"},
                month: {$month: "$posted"},
                week: {$week: "$posted"},
                year: {$year: "$posted"}
            }
        },
        {$sort: {_id: 1}}
    ],
    cursor: {}
});

let p19result = [
    {
        "_id": 1,
        "posted": ISODate("2004-03-21T18:59:54Z"),
        "seconds": 54,
        "minutes": 59,
        "hour": 18,
        "dayOfYear": 81,
        "dayOfMonth": 21,
        "dayOfWeek": 1,
        "month": 3,
        "week": 12,
        "year": 2004,
    },
    {
        "_id": 2,
        "posted": ISODate("2030-08-08T04:11:10Z"),
        "seconds": 10,
        "minutes": 11,
        "hour": 4,
        "dayOfYear": 220,
        "dayOfMonth": 8,
        "dayOfWeek": 5,
        "month": 8,
        "week": 31,
        "year": 2030,
    },
    {
        "_id": 3,
        "posted": ISODate("2000-12-31T05:17:14Z"),
        "seconds": 14,
        "minutes": 17,
        "hour": 5,
        "dayOfYear": 366,
        "dayOfMonth": 31,
        "dayOfWeek": 1,
        "month": 12,
        "week": 53,
        "year": 2000,
    }
];

assert.docEq(p19.cursor.firstBatch, p19result, 'p19 failed');

testDB.lettype.drop();
testDB.lettype.save({x: 17, y: "foo"});

// ternary conditional operator
let p21 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {
            $project: {
                _id: 0,
                author: 1,
                pageViews: {
                    $cond:
                        [{$eq: ["$author", "dave"]}, {$add: ["$pageViews", 1000]}, "$pageViews"]
                }
            }
        },
        {$sort: {author: 1}}
    ],
    cursor: {}
});

let p21result = [
    {"author": "bob", "pageViews": 5},
    {"author": "dave", "pageViews": 1007},
    {"author": "jane", "pageViews": 6}
];

assert.docEq(p21.cursor.firstBatch, p21result, 'p21 failed');

// simple matching
let m1 =
    testDB.runCommand({aggregate: "article", pipeline: [{$match: {author: "dave"}}], cursor: {}});

let m1result = [{
    "_id": 2,
    "title": "this is your title",
    "author": "dave",
    "posted": ISODate("2030-08-08T04:11:10Z"),
    "pageViews": 7,
    "tags": ["fun", "nasty"],
    "comments": [
        {"author": "barbara", "text": "this is interesting"},
        {"author": "jenny", "text": "i like to play pinball", "votes": 10}
    ],
    "other": {"bar": 14}
}];

assert.docEq(m1.cursor.firstBatch, m1result, 'm1 failed');

// combining matching with a projection
let m2 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {$project: {title: 1, author: 1, pageViews: 1, tags: 1, comments: 1}},
        {$unwind: "$tags"},
        {$match: {tags: "nasty"}},
        {$sort: {_id: 1}}
    ],
    cursor: {}
});

let m2result = [
    {
        "_id": 2,
        "title": "this is your title",
        "author": "dave",
        "pageViews": 7,
        "tags": "nasty",
        "comments": [
            {"author": "barbara", "text": "this is interesting"},
            {"author": "jenny", "text": "i like to play pinball", "votes": 10}
        ]
    },
    {
        "_id": 3,
        "title": "this is some other title",
        "author": "jane",
        "pageViews": 6,
        "tags": "nasty",
        "comments": [
            {"author": "will", "text": "i don't like the color"},
            {"author": "jenny", "text": "can i get that in green?"}
        ]
    }
];

assert.docEq(m2.cursor.firstBatch, m2result, 'm2 failed');

// group by tag, _id is a field reference
let g1 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {$project: {author: 1, tags: 1, pageViews: 1}},
        {$unwind: "$tags"},
        {$group: {_id: "$tags", docsByTag: {$sum: 1}, viewsByTag: {$sum: "$pageViews"}}},
        {$sort: {'_id': 1}}
    ],
    cursor: {}
});

let g1result = [
    {"_id": "filthy", "docsByTag": 1, "viewsByTag": 6},
    {"_id": "fun", "docsByTag": 3, "viewsByTag": 17},
    {"_id": "good", "docsByTag": 1, "viewsByTag": 5},
    {"_id": "nasty", "docsByTag": 2, "viewsByTag": 13},
];

assert.docEq(g1.cursor.firstBatch, g1result, 'g1 failed');

// $max, and averaging in a final projection; _id is structured
let g2 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {$project: {author: 1, tags: 1, pageViews: 1}},
        {$unwind: "$tags"},
        {
            $group: {
                _id: {tags: "$tags"},
                docsByTag: {$sum: 1},
                viewsByTag: {$sum: "$pageViews"},
                mostViewsByTag: {$max: "$pageViews"},
            }
        },
        {
            $project: {
                _id: false,
                tag: "$_id.tags",
                mostViewsByTag: 1,
                docsByTag: 1,
                viewsByTag: 1,
                avgByTag: {$divide: ["$viewsByTag", "$docsByTag"]}
            }
        },
        {$sort: {'docsByTag': 1, 'viewsByTag': 1}}
    ],
    cursor: {}
});

let g2result = [
    {"docsByTag": 1, "viewsByTag": 5, "mostViewsByTag": 5, "tag": "good", "avgByTag": 5},
    {"docsByTag": 1, "viewsByTag": 6, "mostViewsByTag": 6, "tag": "filthy", "avgByTag": 6},
    {"docsByTag": 2, "viewsByTag": 13, "mostViewsByTag": 7, "tag": "nasty", "avgByTag": 6.5},
    {
        "docsByTag": 3,
        "viewsByTag": 17,
        "mostViewsByTag": 7,
        "tag": "fun",
        "avgByTag": 5.666666666666667
    }
];

assert.docEq(g2.cursor.firstBatch, g2result, 'g2 failed');

// $push as an accumulator; can pivot data
let g3 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {
            $project: {
                author: 1,
                tags: 1,
            }
        },
        {$unwind: "$tags"},
        {$sort: {author: 1}},
        {$group: {_id: {tags: "$tags"}, authors: {$push: "$author"}}},
        {$sort: {'_id': 1}}
    ],
    cursor: {}
});

let g3result = [
    {"_id": {"tags": "filthy"}, "authors": ["jane"]},
    {"_id": {"tags": "fun"}, "authors": ["bob", "bob", "dave"]},
    {"_id": {"tags": "good"}, "authors": ["bob"]},
    {"_id": {"tags": "nasty"}, "authors": ["dave", "jane"]}
];

assert.docEq(g3.cursor.firstBatch, g3result, 'g3 failed');

// $avg, and averaging in a final projection
let g4 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {$project: {author: 1, tags: 1, pageViews: 1}},
        {$unwind: "$tags"},
        {
            $group: {
                _id: {tags: "$tags"},
                docsByTag: {$sum: 1},
                viewsByTag: {$sum: "$pageViews"},
                avgByTag: {$avg: "$pageViews"},
            }
        },
        {$sort: {'_id': 1}}
    ],
    cursor: {}
});

let g4result = [
    {"_id": {"tags": "filthy"}, "docsByTag": 1, "viewsByTag": 6, "avgByTag": 6},
    {"_id": {"tags": "fun"}, "docsByTag": 3, "viewsByTag": 17, "avgByTag": 5.666666666666667},
    {"_id": {"tags": "good"}, "docsByTag": 1, "viewsByTag": 5, "avgByTag": 5},
    {"_id": {"tags": "nasty"}, "docsByTag": 2, "viewsByTag": 13, "avgByTag": 6.5}
];

assert.docEq(g4.cursor.firstBatch, g4result, 'g4 failed');

// $addToSet as an accumulator; can pivot data
let g5 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {
            $project: {
                author: 1,
                tags: 1,
            }
        },
        {$unwind: "$tags"},
        {$group: {_id: {tags: "$tags"}, authors: {$addToSet: "$author"}}},
        {$sort: {'_id': 1}}
    ],
    cursor: {}
});

// $addToSet doesn't guarantee order so we shouldn't test for it.
g5.cursor.firstBatch.forEach(function(obj) {
    obj.authors.sort();
});

let g5result = [
    {"_id": {"tags": "filthy"}, "authors": ["jane"]},
    {
        "_id": {"tags": "fun"},
        "authors": [
            "bob",
            "dave",
        ]
    },
    {"_id": {"tags": "good"}, "authors": ["bob"]},
    {
        "_id": {"tags": "nasty"},
        "authors": [
            "dave",
            "jane",
        ]
    }
];

assert.docEq(g5.cursor.firstBatch, g5result, 'g5 failed');

// $first and $last accumulators, constant _id
let g6 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {$sort: {author: -1}},
        {
            $group: {
                _id: "authors",                  /* constant string, *not* a field reference */
                firstAuthor: {$last: "$author"}, /* note reverse sort above */
                lastAuthor: {$first: "$author"}, /* note reverse sort above */
                count: {$sum: 1}
            }
        }
    ],
    cursor: {}
});

let g6result = [{"_id": "authors", firstAuthor: "bob", lastAuthor: "jane", count: 3}];

// Test unwind on an unused field
let g7 = testDB.runCommand({
    aggregate: "article",
    pipeline: [
        {$unwind: '$tags'},
        {
            $group: {
                _id: "tag_count", /* constant string, *not* a field reference */
                count: {$sum: 1}
            }
        }
    ],
    cursor: {}
});
assert.eq(g7.cursor.firstBatch[0].count, 7);
}());
