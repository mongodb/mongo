if ( 0 ) {

/* load the test documents */
load('jstests/aggregation/data/articles.js');

/* load the test utilities */
load('jstests/aggregation/extras/utils.js');

function removeVariants(ed) {
    // ignore the timing, since it may vary
    delete ed.serverPipeline[0].cursor.millis;

    // ignore the server the test runs on
    delete ed.serverPipeline[0].cursor.server;
}

/* sample aggregate explain command queries */
// make sure we're using the right db; this is the same as "use mydb;" in shell
db = db.getSisterDB("aggdb");

// $max, and averaging in a final projection; _id is structured
var eg2 = db.runCommand({ aggregate : "article", explain: true,
                          splitMongodPipeline: true, pipeline : [
    { $project : {
        author : 1,
        tags : 1,
        pageViews : 1
    }},
    { $unwind : "$tags" },
    { $group : {
        _id: { tags : 1 },
        docsByTag : { $sum : 1 },
        viewsByTag : { $sum : "$pageViews" },
        mostViewsByTag : { $max : "$pageViews" },
    }},
    { $project : {
        _id: false,
        tag : "$_id.tags",
        mostViewsByTag : 1,
        docsByTag : 1,
        viewsByTag : 1,
        avgByTag : { $divide:["$viewsByTag", "$docsByTag"] }
    }}
]});

removeVariants(eg2);

var eg2result = {
        "serverPipeline" : [
                {
                        "query" : {

                        },
                        "cursor" : {
                                "cursor" : "BasicCursor",
                                "isMultiKey" : false,
                                "n" : 3,
                                "nscannedObjects" : 3,
                                "nscanned" : 3,
                                "scanAndOrder" : false,
                                "indexOnly" : false,
                                "nYields" : 0,
                                "nChunkSkips" : 0,
                                "indexBounds" : {

                                },
                                "allPlans" : [
                                        {
                                                "cursor" : "BasicCursor",
                                                "n" : 3,
                                                "nscannedObjects" : 3,
                                                "nscanned" : 3,
                                                "indexBounds" : {

                                                }
                                        }
                                ]
                        }
                },
                {
                        "$project" : {
                                "author" : true,
                                "pageViews" : true,
                                "tags" : true
                        }
                },
                {
                        "$unwind" : "$tags"
                },
                {
                        "$group" : {
                                "_id" : {
                                        "tags" : true
                                },
                                "docsByTag" : {
                                        "$sum" : 1
                                },
                                "viewsByTag" : {
                                        "$sum" : "$pageViews"
                                },
                                "mostViewsByTag" : {
                                        "$max" : "$pageViews"
                                }
                        }
                }
        ],
        "mongosPipeline" : [
                {
                        "$group" : {
                                "_id" : "$_id",
                                "docsByTag" : {
                                        "$sum" : "$docsByTag"
                                },
                                "viewsByTag" : {
                                        "$sum" : "$viewsByTag"
                                },
                                "mostViewsByTag" : {
                                        "$max" : "$mostViewsByTag"
                                }
                        }
                },
                {
                        "$project" : {
                                "_id" : false,
                                "docsByTag" : true,
                                "mostViewsByTag" : true,
                                "viewsByTag" : true,
                                "tag" : "$_id.tags",
                                "avgByTag" : {
                                        "$divide" : [
                                                "$viewsByTag",
                                                "$docsByTag"
                                        ]
                                }
                        }
                }
        ],
        "ok" : 1
};

assert(documentEq(eg2, eg2result), 'eg2 failed');


db.digits.drop();
for(i = 0; i < 21; i += 2) db.digits.insert( { d : i } );
for(i = 1; i < 20; i += 2) db.digits.insert( { d : i } );

var ed1 = db.runCommand({ aggregate : "digits", explain: true, pipeline : [
    { $match: { d : {$gte : 5, $lte : 15}}}
]});

removeVariants(ed1);

var ed1result = {
        "serverPipeline" : [
                {
                        "query" : {
                                "d" : {
                                        "$gte" : 5,
                                        "$lte" : 15
                                }
                        },
                        "cursor" : {
                                "cursor" : "BasicCursor",
                                "isMultiKey" : false,
                                "n" : 11,
                                "nscannedObjects" : 21,
                                "nscanned" : 21,
                                "scanAndOrder" : false,
                                "indexOnly" : false,
                                "nYields" : 0,
                                "nChunkSkips" : 0,
                                "indexBounds" : {

                                },
                                "allPlans" : [
                                        {
                                                "cursor" : "BasicCursor",
                                                "n" : 11,
                                                "nscannedObjects" : 21,
                                                "nscanned" : 21,
                                                "indexBounds" : {

                                                }
                                        }
                                ]
                        }
                }
        ],
        "ok" : 1
};

assert(documentEq(ed1, ed1result), 'ed1 failed');


var ed2 = db.runCommand({ aggregate : "digits", explain: true, pipeline : [
    { $sort : { d : 1 } },
    { $skip : 5 },
    { $limit : 10 }
]});

removeVariants(ed2);

var ed2result = {
        "serverPipeline" : [
                {
                        "query" : {

                        },
                        "cursor" : {
                                "cursor" : "BasicCursor",
                                "isMultiKey" : false,
                                "n" : 21,
                                "nscannedObjects" : 21,
                                "nscanned" : 21,
                                "scanAndOrder" : false,
                                "indexOnly" : false,
                                "nYields" : 0,
                                "nChunkSkips" : 0,
                                "indexBounds" : {

                                },
                                "allPlans" : [
                                        {
                                                "cursor" : "BasicCursor",
                                                "n" : 21,
                                                "nscannedObjects" : 21,
                                                "nscanned" : 21,
                                                "indexBounds" : {

                                                }
                                        }
                                ]
                        }
                },
                {
                        "$sort" : {
                                "d" : 1
                        }
                },
                {
                        "$skip" : NumberLong(5)
                },
                {
                        "$limit" : NumberLong(10)
                }
        ],
        "ok" : 1
};

assert(documentEq(ed2, ed2result), 'ed2 failed');


var ed3 = db.runCommand({ aggregate : "digits", explain: true, pipeline : [
    { $match: { d : {$gte : 10, $lte : 20}}},
    { $sort : { d : 1 } },
    { $skip : 5 },
    { $limit : 10 }
]});

removeVariants(ed3);

var ed3result = {
        "serverPipeline" : [
                {
                        "query" : {
                                "d" : {
                                        "$gte" : 10,
                                        "$lte" : 20
                                }
                        },
                        "cursor" : {
                                "cursor" : "BasicCursor",
                                "isMultiKey" : false,
                                "n" : 11,
                                "nscannedObjects" : 21,
                                "nscanned" : 21,
                                "scanAndOrder" : false,
                                "indexOnly" : false,
                                "nYields" : 0,
                                "nChunkSkips" : 0,
                                "indexBounds" : {

                                },
                                "allPlans" : [
                                        {
                                                "cursor" : "BasicCursor",
                                                "n" : 11,
                                                "nscannedObjects" : 21,
                                                "nscanned" : 21,
                                                "indexBounds" : {

                                                }
                                        }
                                ]
                        }
                },
                {
                        "$sort" : {
                                "d" : 1
                        }
                },
                {
                        "$skip" : NumberLong(5)
                },
                {
                        "$limit" : NumberLong(10)
                }
        ],
        "ok" : 1
};

assert(documentEq(ed3, ed3result), 'ed3 failed');


/****
Repeat those last three with an index
*****/
db.digits.ensureIndex( { d : 1 } );


var edi1 = db.runCommand({ aggregate : "digits", explain: true, pipeline : [
    { $match: { d : {$gte : 5, $lte : 15}}}
]});

removeVariants(edi1);

var edi1result = {
        "serverPipeline" : [
                {
                        "query" : {
                                "d" : {
                                        "$gte" : 5,
                                        "$lte" : 15
                                }
                        },
                        "cursor" : {
                                "cursor" : "BtreeCursor d_1",
                                "isMultiKey" : false,
                                "n" : 11,
                                "nscannedObjects" : 11,
                                "nscanned" : 11,
                                "scanAndOrder" : false,
                                "indexOnly" : false,
                                "nYields" : 0,
                                "nChunkSkips" : 0,
                                "indexBounds" : {
                                        "d" : [
                                                [
                                                        5,
                                                        15
                                                ]
                                        ]
                                },
                                "allPlans" : [
                                        {
                                                "cursor" : "BtreeCursor d_1",
                                                "n" : 11,
                                                "nscannedObjects" : 11,
                                                "nscanned" : 11,
                                                "indexBounds" : {
                                                        "d" : [
                                                                [
                                                                        5,
                                                                        15
                                                                ]
                                                        ]
                                                }
                                        }
                                ],
                                "oldPlan" : {
                                        "cursor" : "BtreeCursor d_1",
                                        "indexBounds" : {
                                                "d" : [
                                                        [
                                                                5,
                                                                15
                                                        ]
                                                ]
                                        }
                                }
                        }
                }
        ],
        "ok" : 1
};

assert(documentEq(edi1, edi1result), 'edi1 failed');


var edi2 = db.runCommand({ aggregate : "digits", explain: true, pipeline : [
    { $sort : { d : 1 } },
    { $skip : 5 },
    { $limit : 10 }
]});

removeVariants(edi2);

var edi2result = {
    "serverPipeline" : [
        {
            "query" : {
                
            },
            "sort" : {
                "d" : 1
            },
            "cursor" : {
                "cursor" : "BtreeCursor d_1",
                "isMultiKey" : false,
                "n" : 21,
                "nscannedObjects" : 21,
                "nscanned" : 21,
                "scanAndOrder" : false,
                "indexOnly" : false,
                "nYields" : 0,
                "nChunkSkips" : 0,
                "indexBounds" : {
                    "d" : [
                        [
                            {
                                "$minElement" : 1
                            },
                            {
                                "$maxElement" : 1
                            }
                        ]
                    ]
                },
                "allPlans" : [
                    {
                        "cursor" : "BtreeCursor d_1",
                        "n" : 21,
                        "nscannedObjects" : 21,
                        "nscanned" : 21,
                        "indexBounds" : {
                            "d" : [
                                [
                                    {
                                        "$minElement" : 1
                                    },
                                    {
                                        "$maxElement" : 1
                                    }
                                ]
                            ]
                        }
                    }
                ],
                "oldPlan" : {
                    "cursor" : "BtreeCursor d_1",
                    "indexBounds" : {
                        "d" : [
                            [
                                {
                                    "$minElement" : 1
                                },
                                {
                                    "$maxElement" : 1
                                }
                            ]
                        ]
                    }
                }
            }
        },
        {
            "$skip" : NumberLong(5)
        },
        {
            "$limit" : NumberLong(10)
        }
    ],
    "ok" : 1
};

assert(documentEq(edi2, edi2result), 'edi2 failed');


var edi3 = db.runCommand({ aggregate : "digits", explain: true, pipeline : [
    { $match: { d : {$gte : 10, $lte : 20}}},
    { $sort : { d : 1 } },
    { $skip : 5 },
    { $limit : 10 }
]});

removeVariants(edi3);

var edi3result = {
        "serverPipeline" : [
                {
                        "query" : {
                                "d" : {
                                        "$gte" : 10,
                                        "$lte" : 20
                                }
                        },
                        "sort" : {
                                "d" : 1
                        },
                        "cursor" : {
                                "cursor" : "BtreeCursor d_1",
                                "isMultiKey" : false,
                                "n" : 11,
                                "nscannedObjects" : 11,
                                "nscanned" : 11,
                                "scanAndOrder" : false,
                                "indexOnly" : false,
                                "nYields" : 0,
                                "nChunkSkips" : 0,
                                "indexBounds" : {
                                        "d" : [
                                                [
                                                        10,
                                                        20
                                                ]
                                        ]
                                },
                                "allPlans" : [
                                        {
                                                "cursor" : "BtreeCursor d_1",
                                                "n" : 11,
                                                "nscannedObjects" : 11,
                                                "nscanned" : 11,
                                                "indexBounds" : {
                                                        "d" : [
                                                                [
                                                                        10,
                                                                        20
                                                                ]
                                                        ]
                                                }
                                        }
                                ],
                                "oldPlan" : {
                                        "cursor" : "BtreeCursor d_1",
                                        "indexBounds" : {
                                                "d" : [
                                                        [
                                                                10,
                                                                20
                                                        ]
                                                ]
                                        }
                                }
                        }
                },
                {
                        "$skip" : NumberLong(5)
                },
                {
                        "$limit" : NumberLong(10)
                }
        ],
    "ok" : 1
};

assert(documentEq(edi3, edi3result), 'edi3 failed');

}
