/*
  Test the test utilities themselves
*/
var verbose = false;

var t1result = [
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "pageViews" : 5,
        "tags" : [
            "fun",
            "good"
        ]
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "pageViews" : 7,
        "tags" : [
            "fun",
            "nasty"
        ]
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "pageViews" : 6,
        "tags" : [
            "nasty",
            "filthy"
        ]
    }
];

assert(arrayEq(t1result, t1result, verbose), 't0a failed');
assert(resultsEq(t1result, t1result, verbose), 't0b failed');

var t1resultr = [
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "pageViews" : 5,
        "tags" : [
            "fun",
            "good"
        ]
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "pageViews" : 6,
        "tags" : [
            "nasty",
            "filthy"
        ]
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "pageViews" : 7,
        "tags" : [
            "fun",
            "nasty"
        ]
    },
];

assert(resultsEq(t1resultr, t1result, verbose), 'tr1 failed');
assert(resultsEq(t1result, t1resultr, verbose), 'tr2 failed');

var t1resultf1 = [
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "pageViews" : 7,
        "tags" : [
            "fun",
            "nasty"
        ]
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "pageViews" : 6,
        "tags" : [
            "nasty",
            "filthy"
        ]
    }
];

assert(!resultsEq(t1result, t1resultf1, verbose), 't1a failed');
assert(!resultsEq(t1resultf1, t1result, verbose), 't1b failed');

var t1resultf2 = [
    {
        "pageViews" : 5,
        "tags" : [
            "fun",
            "good"
        ]
    },
    {
        "pageViews" : 7,
        "tags" : [
            "fun",
            "nasty"
        ]
    },
    {
        "pageViews" : 6,
        "tags" : [
            "nasty",
            "filthy"
        ]
    }
];

assert(!resultsEq(t1result, t1resultf2, verbose), 't2a failed');
assert(!resultsEq(t1resultf2, t1result, verbose), 't2b failed');

var t1resultf3 = [
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "pageViews" : 5,
        "tags" : [
            "fun",
        ]
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "pageViews" : 7,
        "tags" : [
            "fun",
            "nasty"
        ]
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "pageViews" : 6,
        "tags" : [
            "filthy"
        ]
    }
];

assert(!resultsEq(t1result, t1resultf3, verbose), 't3a failed');
assert(!resultsEq(t1resultf3, t1result, verbose), 't3b failed');
