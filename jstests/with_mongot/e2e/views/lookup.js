/**
 * This file tests lookup with search subpipeline on a combination of collections + views:
 * 1. the outer collection is a collection and the inner collection is a view.
 * 		a. does not include explain() validation, see code comment below
 * 2. the outer collection is a view and the inner collection is a collection.
 *		a. includes explain() validation
 * 3. the outer collection is a view and the inner collection is a view.
 * 		a. includes explain() validation
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    checkSbeFullyEnabled,
} from "jstests/libs/query/sbe_util.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e/lib/explain_utils.js";

// Setup for all three use cases.
const productColl = db[`${jsTest.name()}`];
const reviewColl = db[`${jsTest.name()}_foreign`];
productColl.drop();
reviewColl.drop();
// Need for explain assertions.
const isSbeEnabled = checkSbeFullyEnabled(db);

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

// First use case: outer coll is a regular collection, inner coll is a view.
// ========================================================================

/**
 * Create a view on the reviewColl that adds a verified_review field if null in the underlying
 * source collection.
 */
let viewName = "verifiedReview";
let verifiedReviewViewPipeline =
    [{"$addFields": {verified_review: {$ifNull: ['$verified_review', 'unverified']}}}];
assert.commandWorked(
    db.createView("verifiedReview", reviewColl.getName(), verifiedReviewViewPipeline));
let verifiedReviewView = db["verifiedReview"];

createSearchIndex(verifiedReviewView,
                  {name: "verifiedReviewSearchIndex", definition: {"mappings": {"dynamic": true}}});

let searchQuery = {index: "verifiedReviewSearchIndex", text: {query: "battery", path: "review"}};

let lookupPipeline = [
    {
        $lookup: {
            from: "verifiedReview",
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
/**
 * $lookup doesn't include any details about its subpipeline in the explain output. More concretely,
 * its serialize function returns the stored _userPipeline which is the pipeline prior to any
 * optimizations/view resolution. In other words, $search will not be desugared and therefore
 * DocumentSourceInternalSearchIdLookup::serialize(), which publicizies the view transforms for
 * mongot pipelines, will never be invoked. This is relevant for testing because we cannot examine
 * the explain output for the view transforms for $lookup.$search when the inner coll is a view -
 * because again, the view transforms will not be present.
 */

let results = productColl.aggregate(lookupPipeline).toArray();
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
assertArrayEq({actual: results, expected: expectedResults});

// Second use case: outer coll is a view, inner coll is a regular collection.
// ==========================================================================
createSearchIndex(reviewColl, {name: "default", definition: {"mappings": {"dynamic": true}}});
let batteryTypeViewPipeline =
    [{"$addFields": {battery_type: {$ifNull: ['$battery_type', 'unknown']}}}];
assert.commandWorked(
    db.createView("batteryTypeView", productColl.getName(), batteryTypeViewPipeline));
let batteryTypeViewOnProductColl = db["batteryTypeView"];

searchQuery = {
    index: "default",
    text: {query: "battery", path: "review"}
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
}];
/**
 * Since only the outer collection is a view, we are able to validate that the user pipeline was
 * appended to end of the view pipeline.
 */
let explain =
    assert.commandWorked(batteryTypeViewOnProductColl.explain().aggregate(lookupPipeline));

if (isSbeEnabled) {
    assertViewAppliedCorrectly(explain.command.pipeline, lookupPipeline, batteryTypeViewPipeline);
} else {
    /**
     * The first stage is a $cursor, which represents the intermediate results of the outer coll
     * that will be streamed through the rest of the pipeline. But we don't need it to validate how
     * the view was applied.
     */
    explain.stages.shift();
    assertViewAppliedCorrectly(explain.stages, lookupPipeline, batteryTypeViewPipeline);
}

results = batteryTypeViewOnProductColl.aggregate(lookupPipeline).toArray();
/**
 * There should be no [verified_review : 'unverified' ] field/values in these results, as that is
 * associated with the view on reviewColl. However, there should be battery_type field for every
 * element as the outer coll is a view that adds that field if missing in the underlying source
 * coll.
 */
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

assertArrayEq({actual: results, expected: expectedResults});

// Third use case: outer coll is a view, inner coll is a view.
// ===========================================================
createSearchIndex(batteryTypeViewOnProductColl,
                  {name: "default", definition: {"mappings": {"dynamic": true}}});
searchQuery = {
    index: "default",
    text: {query: "battery", path: "instructions"}
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
{$project: {
    instructions: 0, 
	battery_type: 0,
    _id: 0
}}
];

/**
 * We are able to validate that the user pipeline was appended to the view pipeline for the outer
 * collection. However, we are not able to validate via explain that the view pipeline was applied
 * by idLookup. See comment under first use case about $lookup serialize function to understand why.
 */
explain = assert.commandWorked(verifiedReviewView.explain().aggregate(lookupPipeline));

if (isSbeEnabled) {
    assertViewAppliedCorrectly(
        explain.command.pipeline, lookupPipeline, verifiedReviewViewPipeline);
} else {
    /**
     *  The first stage is a $cursor, which represents the intermediate results of the outer coll
     * that will be streamed through the rest of the pipeline. But we don't need it to validate how
     * the view was applied.
     */
    explain.stages.shift();
    assertViewAppliedCorrectly(explain.stages, lookupPipeline, verifiedReviewViewPipeline);
}

/**
 * There should be a verified_review field (for the outer view) and battery_type field (for the
 * inner view) for each element in the results
 * */
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

results = verifiedReviewView.aggregate(lookupPipeline).toArray();
assertArrayEq({actual: results, expected: expectedResults});
dropSearchIndex(reviewColl, {name: "default"});
dropSearchIndex(verifiedReviewView, {name: "verifiedReviewSearchIndex"});
dropSearchIndex(batteryTypeViewOnProductColl, {name: "default"});