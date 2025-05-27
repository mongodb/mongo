/**
 * This file tests $lookup with $search subpipelines on a combination of collections + views:
 * 1. The outer collection is a collection and the inner collection is a view.
 * 2. The outer collection is a view and the inner collection is a collection.
 * 3. The outer collection is a view and the inner collection is a view.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertLookupInExplain} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    createSearchIndexesAndExecuteTests,
    validateSearchExplain,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const productColl = testDb.products;
const reviewColl = testDb.reviews;
productColl.drop();
reviewColl.drop();

assert.commandWorked(productColl.insertMany([
    {
        _id: 3,
        name: "smoke detector",
        instructions: "battery not included",
        battery_type: "AAA",
        category: "electronics"
    },
    {
        _id: 4,
        name: "solar clock",
        instructions: "put it in direct sunlight",
        category: "home goods"
    },
    {
        _id: 5,
        name: "tv remote",
        instructions: "free rechargeable battery if you buy 10 remotes",
        category: "electronics"
    }
]));

assert.commandWorked(reviewColl.insertMany([
    {
        _id: 1,
        productId: 5,
        review: "Easy to setup and great battery life!",
        verified_review: "verified by shopify bot"
    },
    {_id: 2, productId: 4, review: "Pretty but not worth the price."},
    {
        _id: 3,
        productId: 4,
        review: "Excellent sound quality when the clock strikes one!",
        verified_review: "verified by shopify bot"
    },
    {_id: 4, productId: 3, review: "Battery drains quickly."},
    {
        _id: 5,
        productId: 3,
        review: "Worthless battery - had to replace after two weeks.",
        verified_review: "verified by shopify bot"
    },
    {
        _id: 6,
        productId: 3,
        review:
            "Don't know what ppl are saying - perfectly fine battery. Maybe your battery was out of juice from the start. Try buying new batteries"
    }
]));

// Create views on the review and product collections.
const verifiedReviewViewName = "verifiedReview";
const verifiedReviewViewPipeline =
    [{"$addFields": {verified_review: {$ifNull: ['$verified_review', 'unverified']}}}];
assert.commandWorked(
    testDb.createView(verifiedReviewViewName, reviewColl.getName(), verifiedReviewViewPipeline));
const verifiedReviewView = testDb[verifiedReviewViewName];

const batteryTypeViewName = "batteryTypeView";
const batteryTypeViewPipeline =
    [{"$addFields": {battery_type: {$ifNull: ['$battery_type', 'unknown']}}}];
assert.commandWorked(
    testDb.createView(batteryTypeViewName, productColl.getName(), batteryTypeViewPipeline));
const batteryTypeViewOnProductColl = testDb[batteryTypeViewName];

// Configure search indexes for all collections/views.
const indexConfigs = [
    {
        coll: verifiedReviewView,
        definition: {name: "verifiedReviewSearchIndex", definition: {"mappings": {"dynamic": true}}}
    },
    {coll: reviewColl, definition: {name: "default", definition: {"mappings": {"dynamic": true}}}},
    {
        coll: batteryTypeViewOnProductColl,
        definition: {name: "default", definition: {"mappings": {"dynamic": true}}}
    }
];

const lookupTestCases = (isStoredSource) => {
    // ===============================================================================
    // Case 1: Outer coll is a regular collection, inner coll is a view.
    // ===============================================================================
    let searchQuery = {
        index: "verifiedReviewSearchIndex",
        text: {query: "battery", path: "review"},
        returnStoredSource: isStoredSource
    };

    let lookupPipeline = [
            {
                $lookup: {
                    from: verifiedReviewViewName,
                    localField: "_id",
                    foreignField: "productId",
                    pipeline: [
                        {$search: searchQuery},
                        {$project: {_id: 0}}
                    ],
                    as: "batteryReviews"
                },
            }, 
            {
                $project: {
                    instructions: 0, 
                    battery_type: 0
                }
            }
        ];

    let expectedResults = [
        {
            _id: 3,
            name: "smoke detector",
            category: "electronics",
            batteryReviews: [
                {productId: 3, review: "Battery drains quickly.", verified_review: "unverified"},
                {
                    productId: 3,
                    review: "Worthless battery - had to replace after two weeks.",
                    verified_review: "verified by shopify bot"
                },
                {
                    productId: 3,
                    review:
                        "Don't know what ppl are saying - perfectly fine battery. Maybe your battery was out of juice from the start. Try buying new batteries",
                    verified_review: "unverified"
                }
            ]
        },
        {_id: 4, name: "solar clock", category: "home goods", batteryReviews: []},
        {
            _id: 5,
            name: "tv remote",
            category: "electronics",
            batteryReviews: [{
                productId: 5,
                review: "Easy to setup and great battery life!",
                verified_review: "verified by shopify bot"
            }]
        }
    ];

    // $lookup doesn't include any details about its subpipeline in the explain output. More
    // concretely, its serialize function returns the stored _userPipeline which is the pipeline
    // prior to any optimizations/view resolution. In other words, $search will not be desugared and
    // therefore DocumentSourceInternalSearchIdLookup::serialize(), which publicizies the view
    // transforms for mongot pipelines, will never be invoked. This is relevant for testing because
    // we cannot examine the explain output for the view transforms for $lookup.$search when the
    // inner coll is a view - because again, the view transforms will not be present.
    validateSearchExplain(productColl, lookupPipeline, isStoredSource, null, (explain) => {
        assertLookupInExplain(explain, lookupPipeline[0]);
    });

    let results = productColl.aggregate(lookupPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    // ===============================================================================
    // Case 2: Outer coll is a view, inner coll is a regular collection.
    // ===============================================================================
    searchQuery = {
        index: "default",
        text: {query: "battery", path: "review"},
        returnStoredSource: isStoredSource
    };

    lookupPipeline = [
                {
                    $lookup: {
                        from: reviewColl.getName(),
                        localField: "_id",
                        foreignField: "productId",
                        pipeline: [
                            {$search: searchQuery},
                        ],
                        as: "batteryReviews"
                    }
                }
            ];

    // There should be no [verified_review : 'unverified' ] field/values in these results, as that
    // is associated with the view on reviewColl. However, there should be battery_type fields for
    // every element as the outer coll is a view that adds that field if missing in the underlying
    // source coll.
    expectedResults = [
        {
            _id: 3,
            name: "smoke detector",
            instructions: "battery not included",
            battery_type: "AAA",
            category: "electronics",
            batteryReviews: [
                {_id: 4, productId: 3, review: "Battery drains quickly."},
                {
                    _id: 5,
                    productId: 3,
                    review: "Worthless battery - had to replace after two weeks.",
                    verified_review: "verified by shopify bot"
                },
                {
                    _id: 6,
                    productId: 3,
                    review:
                        "Don't know what ppl are saying - perfectly fine battery. Maybe your battery was out of juice from the start. Try buying new batteries"
                }
            ]
        },
        {
            _id: 4,
            name: "solar clock",
            instructions: "put it in direct sunlight",
            category: "home goods",
            battery_type: "unknown",
            batteryReviews: []
        },
        {
            _id: 5,
            name: "tv remote",
            instructions: "free rechargeable battery if you buy 10 remotes",
            category: "electronics",
            battery_type: "unknown",
            batteryReviews: [{
                _id: 1,
                productId: 5,
                review: "Easy to setup and great battery life!",
                verified_review: "verified by shopify bot"
            }]
        }
    ];

    validateSearchExplain(batteryTypeViewOnProductColl,
                          lookupPipeline,
                          isStoredSource,
                          batteryTypeViewPipeline,
                          (explain) => {
                              assertLookupInExplain(explain, lookupPipeline[0]);
                          });

    results = batteryTypeViewOnProductColl.aggregate(lookupPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    // ===============================================================================
    // Case 3: Outer coll is a view, inner coll is a view.
    // ===============================================================================
    searchQuery = {
        index: "default",
        text: {query: "battery", path: "instructions"},
        returnStoredSource: isStoredSource
    };

    lookupPipeline = [
                {
                    $lookup: {
                        from: batteryTypeViewOnProductColl.getName(),
                        localField: "productId",
                        foreignField: "_id",
                        pipeline: [
                            {$search: searchQuery}
                        ],
                        as: "batteryReviews"
                    },
                },
                {
                    $project: {
                        instructions: 0,
                        battery_type: 0,
                        _id: 0
                    }
                }
            ];

    // There should be a verified_review field (for the outer view) and battery_type field (for the
    // inner view) for each element in the results.
    expectedResults = [
        {
            productId: 5,
            review: "Easy to setup and great battery life!",
            verified_review: "verified by shopify bot",
            batteryReviews: [{
                _id: 5,
                name: "tv remote",
                instructions: "free rechargeable battery if you buy 10 remotes",
                category: "electronics",
                battery_type: "unknown"
            }]
        },
        {
            productId: 4,
            review: "Pretty but not worth the price.",
            verified_review: "unverified",
            batteryReviews: []
        },
        {
            productId: 4,
            review: "Excellent sound quality when the clock strikes one!",
            verified_review: "verified by shopify bot",
            batteryReviews: []
        },
        {
            productId: 3,
            review: "Battery drains quickly.",
            verified_review: "unverified",
            batteryReviews: [{
                _id: 3,
                name: "smoke detector",
                instructions: "battery not included",
                battery_type: "AAA",
                category: "electronics"
            }]
        },
        {
            productId: 3,
            review: "Worthless battery - had to replace after two weeks.",
            verified_review: "verified by shopify bot",
            batteryReviews: [{
                _id: 3,
                name: "smoke detector",
                instructions: "battery not included",
                battery_type: "AAA",
                category: "electronics"
            }]
        },
        {
            productId: 3,
            review:
                "Don't know what ppl are saying - perfectly fine battery. Maybe your battery was out of juice from the start. Try buying new batteries",
            verified_review: "unverified",
            batteryReviews: [{
                _id: 3,
                name: "smoke detector",
                instructions: "battery not included",
                battery_type: "AAA",
                category: "electronics"
            }]
        }
    ];

    validateSearchExplain(verifiedReviewView,
                          lookupPipeline,
                          isStoredSource,
                          verifiedReviewViewPipeline,
                          (explain) => {
                              assertLookupInExplain(explain, lookupPipeline[0]);
                          });

    results = verifiedReviewView.aggregate(lookupPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});
};

createSearchIndexesAndExecuteTests(indexConfigs, lookupTestCases);
