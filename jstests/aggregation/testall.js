/*
  Run all the aggregation tests
*/

/* load the test documents */
load('articles.js');

/* load the test utilities */
load('utils.js');

// make sure we're using the right db; this is the same as "use mydb;" in shell
db = db.getSisterDB("aggdb");


// just passing through fields
var p1 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	tags : 1,
	pageViews : 1
    }}
]});

var p1result = [
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

assert(arrayEq(p1.result, p1result), 'p1 failed');


// unwinding an array
var p2 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" },
	pageViews : 1
    }}
]});

var p2result = [
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "author" : "bob",
        "pageViews" : 5,
        "tag" : "fun"
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "author" : "bob",
        "pageViews" : 5,
        "tag" : "good"
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "author" : "dave",
        "pageViews" : 7,
        "tag" : "fun"
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "author" : "dave",
        "pageViews" : 7,
        "tag" : "nasty"
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "author" : "jane",
        "pageViews" : 6,
        "tag" : "nasty"
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "author" : "jane",
        "pageViews" : 6,
        "tag" : "filthy"
    }
];

assert(arrayEq(p2.result, p2result), 'p2 failed');


// pulling values out of subdocuments
var p3 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	otherfoo : "other.foo",
	otherbar : "other.bar"
    }}
]});

var p3result = [
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "otherfoo" : 5
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "otherbar" : 14
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "otherbar" : 14
    }
];

assert(arrayEq(p3.result, p3result), 'p3 failed');


// projection includes a computed value
var p4 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	daveWroteIt : { $eq:["$author", "dave"] }
    }}
]});

var p4result = [
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "author" : "bob",
        "daveWroteIt" : false
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "author" : "dave",
        "daveWroteIt" : true
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "author" : "jane",
        "daveWroteIt" : false
    }
];

assert(arrayEq(p4.result, p4result), 'p4 failed');


// projection includes a virtual (fabricated) document
var p5 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	pageViews : 1,
	tag : { $unwind : "tags" }
    }},
    { $project : {
	author : 1,
	subDocument : { foo : "pageViews", bar : "tag"  }
    }}
]});

var p5result = [
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "author" : "bob",
        "subDocument" : {
            "foo" : 5,
            "bar" : "fun"
        }
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "author" : "bob",
        "subDocument" : {
            "foo" : 5,
            "bar" : "good"
        }
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "author" : "dave",
        "subDocument" : {
            "foo" : 7,
            "bar" : "fun"
        }
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "author" : "dave",
        "subDocument" : {
            "foo" : 7,
            "bar" : "nasty"
        }
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "author" : "jane",
        "subDocument" : {
            "foo" : 6,
            "bar" : "nasty"
        }
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "author" : "jane",
        "subDocument" : {
            "foo" : 6,
            "bar" : "filthy"
        }
    }
];

assert(arrayEq(p5.result, p5result), 'p5 failed');


// multi-step aggregate
// nested expressions in computed fields
var p6 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" },
	pageViews : 1
    }},
    { $project : {
	author : 1,
	tag : 1,
	pageViews : 1,
	daveWroteIt : { $eq:["$author", "dave"] },
	weLikeIt : { $or:[ { $eq:["$author", "dave"] },
			   { $eq:["$tag", "good"] } ] }
    }}
]});

var p6result = [
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "author" : "bob",
        "pageViews" : 5,
        "tag" : "fun",
        "daveWroteIt" : false,
        "weLikeIt" : false
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "author" : "bob",
        "pageViews" : 5,
        "tag" : "good",
        "daveWroteIt" : false,
        "weLikeIt" : true
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "author" : "dave",
        "pageViews" : 7,
        "tag" : "fun",
        "daveWroteIt" : true,
        "weLikeIt" : true
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "author" : "dave",
        "pageViews" : 7,
        "tag" : "nasty",
        "daveWroteIt" : true,
        "weLikeIt" : true
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "author" : "jane",
        "pageViews" : 6,
        "tag" : "nasty",
        "daveWroteIt" : false,
        "weLikeIt" : false
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "author" : "jane",
        "pageViews" : 6,
        "tag" : "filthy",
        "daveWroteIt" : false,
        "weLikeIt" : false
    }
];

assert(arrayEq(p6.result, p6result), 'p6 failed');


// slightly more complex computed expression; $ifnull
var p7 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	theSum : { $add:["$pageViews",
			 { $ifnull:["$other.foo",
				    "$other.bar"] } ] }
    }}
]});

// dotted path inclusion; _id exclusion
var p8 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	_id : 0,
	author : 1,
	tag : { $unwind : "tags" },
	"comments.author" : 1
    }}
]});


// simple matching
var m1 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $match : { author : "dave" } }
]});

// combining matching with a projection
var m2 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	title : 1,
	author : 1,
	pageViews : 1,
	tag : { $unwind : "tags" },
	comments : 1
    }},
    { $match : { tag : "nasty" } }
]});


// group by tag
var g1 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" },
	pageViews : 1
    }},
    { $group : {
	_id: { tag : 1 },
	docsByTag : { $sum : 1 },
	viewsByTag : { $sum : "$pageViews" }
    }}
]});

// $max, and averaging in a final projection
var g2 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" },
	pageViews : 1
    }},
    { $group : {
	_id: { tag : 1 },
	docsByTag : { $sum : 1 },
	viewsByTag : { $sum : "$pageViews" },
	mostViewsByTag : { $max : "$pageViews" },
    }},
    { $project : {
	_id: false,
	tag : "_id.tag",
	mostViewsByTag : 1,
	docsByTag : 1,
	viewsByTag : 1,
	avgByTag : { $divide:["$viewsByTag", "$docsByTag"] }
    }}
]});

// $push as an accumulator; can pivot data
var g3 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" }
    }},
    { $group : {
	_id : { tag : 1 },
	authors : { $push : "$author" }
    }}
]});

// $avg, and averaging in a final projection
var g4 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" },
	pageViews : 1
    }},
    { $group : {
	_id: { tag : 1 },
	docsByTag : { $sum : 1 },
	viewsByTag : { $sum : "$pageViews" },
	avgByTag : { $avg : "$pageViews" },
    }}
]});
