/**
 * Tests the use of $searchMeta in a subpipeline--namely, in $lookup and in $unionWith.
 */

const coll = db[jsTestName()];
coll.drop();
const numDocs = 10000;
let docs = [];

let genres = [
    "Drama",
    "Comedy",
    "Romance",
];
for (let i = 0; i < numDocs; i++) {
    const genre = genres[i % 10];
    docs.push({_id: i, index: i % 1000, element: "fire", genre: genre});
}
assert.commandWorked(coll.insertMany(docs));

coll.createSearchIndex({
    name: "facet-index",
    definition: {
        "mappings": {
            "dynamic": false,
            "fields": {"index": {"type": "number"}, "genre": {"type": "stringFacet"}}
        }
    }
});

const countQuery = {
    "$searchMeta": {
        "index": "facet-index",
        "range": {"path": "index", "gte": 100, "lt": 9000},
        "count": {"type": "total"}
    }
};

// Testing $searchMeta in an independent pipeline.
let expectedBasic = [{"count": {"total": NumberLong(9000)}}];
let resultBasic = coll.aggregate(countQuery).toArray();
assert.eq(resultBasic, expectedBasic);

// Create an additional collection for $lookup and $unionWith queries.
const collBase = db.base;
collBase.drop();
assert.commandWorked(collBase.insert({"_id": 100, "localField": "cakes", "weird": false}));

// Test $searchMeta with $lookup.
const lookupPipeline = [
    {$match: {_id: 100}},
    {$lookup: {from: coll.getName(), pipeline: [countQuery], as: "meta_facet"}}
];
let expectedLookup =
    [{"_id": 100, "localField": "cakes", "weird": false, "meta_facet": expectedBasic}];
let resultLookup = collBase.aggregate(lookupPipeline).toArray();
assert.sameMembers(resultLookup, expectedLookup);

// Test $searchMeta with $unionWith.
const unionWithPipeline = [
    {$match: {_id: 100}},
    {
        $unionWith: {
            coll: coll.getName(),
            pipeline: [countQuery],
        }
    }
];
let expectedUnionWith = [{"_id": 100, "localField": "cakes", "weird": false}].concat(expectedBasic);
let resultUnionWith = collBase.aggregate(unionWithPipeline).toArray();
assert.sameMembers(resultUnionWith, expectedUnionWith);

const unionPlusMatchPipeline = [
    {$match: {_id: 100}},
    {
        $unionWith: {
            coll: coll.getName(),
            pipeline: [countQuery],
        }
    },
    {$match: {"count.total": {$exists: false}}}
];
expectedUnionWith = [{"_id": 100, "localField": "cakes", "weird": false}];
resultUnionWith = collBase.aggregate(unionPlusMatchPipeline).toArray();
assert.sameMembers(resultUnionWith, expectedUnionWith);

// Test with it in a bunch of places!
const superNestPipeline = [
    {$match: {_id: 100}},
    {
        $lookup: {
            from: coll.getName(),
            pipeline: [countQuery, {$unionWith: {coll: coll.getName(), pipeline: [countQuery]}}],
            as: "lookup_then_unionWith"
        }
    },
    {
        $unionWith: {
            coll: coll.getName(),
            pipeline: [
                countQuery,
                {$lookup: {from: coll.getName(), pipeline: [countQuery], as:
                "union_then_lookup"}}
            ],
        }
    },
    {
        $lookup: {
            from: coll.getName(),
            pipeline: [
                countQuery,
                {$lookup: {from: coll.getName(), pipeline: [countQuery], as:
                "lookup_then_lookup(nested)"}}
            ],
            as: "lookup_then_lookup(top)"
        }
    },
    {
        $unionWith: {
            coll: coll.getName(),
            pipeline: [
                countQuery,
                {$unionWith: {coll: coll.getName(), pipeline: [countQuery]}}
            ],
        }
    },
    {$project: {_id: 0, weird: 0}},
    {$set: {"i got to the end": true}},
];

let expectedSuperNested = [
    {
        "localField": "cakes",
        "lookup_then_unionWith":
            [{"count": {"total": NumberLong(9000)}}, {"count": {"total": NumberLong(9000)}}],
        "lookup_then_lookup(top)": [{
            "count": {"total": NumberLong(9000)},
            "lookup_then_lookup(nested)": [{"count": {"total": NumberLong(9000)}}]
        }],
        "i got to the end": true
    },
    {
        "count": {"total": NumberLong(9000)},
        "union_then_lookup": [{"count": {"total": NumberLong(9000)}}],
        "lookup_then_lookup(top)": [{
            "count": {"total": NumberLong(9000)},
            "lookup_then_lookup(nested)": [{"count": {"total": NumberLong(9000)}}]
        }],
        "i got to the end": true
    },
    {"count": {"total": NumberLong(9000)}, "i got to the end": true},
    {"count": {"total": NumberLong(9000)}, "i got to the end": true},
];
let resultSuperNested = collBase.aggregate(superNestPipeline).toArray();
assert.sameMembers(resultSuperNested, expectedSuperNested);
