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

var p7result = [
    {
        "_id" : ObjectId("4de54958bf1505139918fce6"),
        "theSum" : 10
    },
    {
        "_id" : ObjectId("4de54958bf1505139918fce7"),
        "theSum" : 21
    },
    {
        "_id" : ObjectId("4de54958bf1505139918fce8"),
        "theSum" : 20
    }
];

assert(arrayEq(p7.result, p7result), 'p7 failed');


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

var p8result = [
    {
        "author" : "bob",
        "comments" : [
            {
                "author" : "joe"
            },
            {
                "author" : "sam"
            }
        ],
        "tag" : "fun"
    },
    {
        "author" : "bob",
        "comments" : [
            {
                "author" : "joe"
            },
            {
                "author" : "sam"
            }
        ],
        "tag" : "good"
    },
    {
        "author" : "dave",
        "comments" : [
            {
                "author" : "barbarella"
            },
            {
                "author" : "leia"
            }
        ],
        "tag" : "fun"
    },
    {
        "author" : "dave",
        "comments" : [
            {
                "author" : "barbarella"
            },
            {
                "author" : "leia"
            }
        ],
        "tag" : "nasty"
    },
    {
        "author" : "jane",
        "comments" : [
            {
                "author" : "r2"
            },
            {
                "author" : "leia"
            }
        ],
        "tag" : "nasty"
    },
    {
        "author" : "jane",
        "comments" : [
            {
                "author" : "r2"
            },
            {
                "author" : "leia"
            }
        ],
        "tag" : "filthy"
    }
];

assert(arrayEq(p8.result, p8result), 'p8 failed');


// collapse a dotted path with an intervening array
var p9 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	_id : 0,
	author : 1,
	commentsAuthor : "comments.author"
    }}
]});

var p9result = [
    {
        "author" : "bob",
        "commentsAuthor" : [
            "joe",
            "sam"
        ]
    },
    {
        "author" : "dave",
        "commentsAuthor" : [
            "barbarella",
            "leia"
        ]
    },
    {
        "author" : "jane",
        "commentsAuthor" : [
            "r2",
            "leia"
        ]
    }
];

assert(arrayEq(p9.result, p9result), 'p9 failed');


// simple sort
var p10 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $sort : {
	title : 1
    }}
]});

var p10result = [
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066d"),
        "title" : "this is my title",
        "author" : "bob",
        "posted" : ISODate("2011-05-03T22:21:33.251Z"),
        "pageViews" : 5,
        "tags" : [
            "fun",
            "good"
        ],
        "comments" : [
            {
                "author" : "joe",
                "text" : "this is cool"
            },
            {
                "author" : "sam",
                "text" : "this is bad"
            }
        ],
        "other" : {
            "foo" : 5
        }
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066f"),
        "title" : "this is some other title",
        "author" : "jane",
        "posted" : ISODate("2011-05-03T22:21:33.252Z"),
        "pageViews" : 6,
        "tags" : [
            "nasty",
            "filthy"
        ],
        "comments" : [
            {
                "author" : "r2",
                "text" : "beep boop"
            },
            {
                "author" : "leia",
                "text" : "this is too smutty"
            }
        ],
        "other" : {
            "bar" : 14
        }
    },
    {
        "_id" : ObjectId("4dc07fedd8420ab8d0d4066e"),
        "title" : "this is your title",
        "author" : "dave",
        "posted" : ISODate("2011-05-03T22:21:33.251Z"),
        "pageViews" : 7,
        "tags" : [
            "fun",
            "nasty"
        ],
        "comments" : [
            {
                "author" : "barbarella",
                "text" : "this is hot"
            },
            {
                "author" : "leia",
                "text" : "i prefer the brass bikini",
                "votes" : 10
            }
        ],
        "other" : {
            "bar" : 14
        }
    }
];

assert(orderedArrayEq(p10.result, p10result), 'p10 failed');


// simple matching
var m1 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $match : { author : "dave" } }
]});

var m1result = [
    {
        "_id" : ObjectId("4de54958bf1505139918fce7"),
        "title" : "this is your title",
        "author" : "dave",
        "posted" : ISODate("2011-05-31T20:02:32.256Z"),
        "pageViews" : 7,
        "tags" : [
            "fun",
            "nasty"
        ],
        "comments" : [
            {
                "author" : "barbarella",
                "text" : "this is hot"
            },
            {
                "author" : "leia",
                "text" : "i prefer the brass bikini",
                "votes" : 10
            }
        ],
        "other" : {
            "bar" : 14
        }
    }
];

assert(arrayEq(m1.result, m1result), 'm1 failed');


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

var m2result = [
    {
        "_id" : ObjectId("4de54958bf1505139918fce7"),
        "title" : "this is your title",
        "author" : "dave",
        "pageViews" : 7,
        "comments" : [
            {
                "author" : "barbarella",
                "text" : "this is hot"
            },
            {
                "author" : "leia",
                "text" : "i prefer the brass bikini",
                "votes" : 10
            }
        ],
        "tag" : "nasty"
    },
    {
        "_id" : ObjectId("4de54958bf1505139918fce8"),
        "title" : "this is some other title",
        "author" : "jane",
        "pageViews" : 6,
        "comments" : [
            {
                "author" : "r2",
                "text" : "beep boop"
            },
            {
                "author" : "leia",
                "text" : "this is too smutty"
            }
        ],
        "tag" : "nasty"
    }
];

assert(arrayEq(m2.result, m2result), 'm2 failed');


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

var g1result = [
    {
        "_id" : {
            "tag" : "filthy"
        },
        "docsByTag" : 1,
        "viewsByTag" : 6
    },
    {
        "_id" : {
            "tag" : "fun"
        },
        "docsByTag" : 2,
        "viewsByTag" : 12
    },
    {
        "_id" : {
            "tag" : "good"
        },
        "docsByTag" : 1,
        "viewsByTag" : 5
    },
    {
        "_id" : {
            "tag" : "nasty"
        },
        "docsByTag" : 2,
        "viewsByTag" : 13
    }
];

assert(arrayEq(g1.result, g1result), 'g1 failed');


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

var g2result = [
    {
        "docsByTag" : 1,
        "viewsByTag" : 6,
        "mostViewsByTag" : 6,
        "tag" : "filthy",
        "avgByTag" : 6
    },
    {
        "docsByTag" : 2,
        "viewsByTag" : 12,
        "mostViewsByTag" : 7,
        "tag" : "fun",
        "avgByTag" : 6
    },
    {
        "docsByTag" : 1,
        "viewsByTag" : 5,
        "mostViewsByTag" : 5,
        "tag" : "good",
        "avgByTag" : 5
    },
    {
        "docsByTag" : 2,
        "viewsByTag" : 13,
        "mostViewsByTag" : 7,
        "tag" : "nasty",
        "avgByTag" : 6.5
    }
];

assert(arrayEq(g2.result, g2result), 'g2 failed');


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

var g3result = [
    {
        "_id" : {
            "tag" : "filthy"
        },
        "authors" : [
            "jane"
        ]
    },
    {
        "_id" : {
            "tag" : "fun"
        },
        "authors" : [
            "bob",
            "dave"
        ]
    },
    {
        "_id" : {
            "tag" : "good"
        },
        "authors" : [
            "bob"
        ]
    },
    {
        "_id" : {
            "tag" : "nasty"
        },
        "authors" : [
            "dave",
            "jane"
        ]
    }
];

assert(arrayEq(g3.result, g3result), 'g3 failed');


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

var g4result = [
    {
        "_id" : {
            "tag" : "filthy"
        },
        "docsByTag" : 1,
        "viewsByTag" : 6,
        "avgByTag" : 6
    },
    {
        "_id" : {
            "tag" : "fun"
        },
        "docsByTag" : 2,
        "viewsByTag" : 12,
        "avgByTag" : 6
    },
    {
        "_id" : {
            "tag" : "good"
        },
        "docsByTag" : 1,
        "viewsByTag" : 5,
        "avgByTag" : 5
    },
    {
        "_id" : {
            "tag" : "nasty"
        },
        "docsByTag" : 2,
        "viewsByTag" : 13,
        "avgByTag" : 6.5
    }
];

assert(arrayEq(g4.result, g4result), 'g4 failed');
