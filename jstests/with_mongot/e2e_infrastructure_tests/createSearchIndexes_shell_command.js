
// Tests for createSearchIndexes shell command

// The following tests include: a simple index test, sort test, facet index test, highlight index
// test, paginating over results test and stored source index tests. These tests use the following
// sample inventory collection with different items.
const coll = db[jsTestName()];

coll.drop();

coll.insert({
    _id: 0,
    item: "Shoes",
    description: "Running shoes",
    color: "red",
    size: 10,
    discontinued: true
});
coll.insert(
    {_id: 1, item: "Phone", description: "Smart phone", color: "black", discontinued: false});
coll.insert(
    {_id: 2, item: "Chocolate", description: "Assorted chocolate box", discontinued: false});
coll.insert({
    _id: 3,
    item: "Dress",
    description: "Designer wear for special occasions",
    color: "black",
    discontinued: true
});
coll.insert({
    _id: 4,
    item: "Dress",
    description: "Formal collection for work",
    color: "crimson red",
    discontinued: false
});

coll.insert({
    _id: 5,
    item: "Shoes",
    description: "Soccer shoes",
    color: "white",
    size: 10,
    discontinued: false
});
coll.insert({
    _id: 6,
    item: "Shoes",
    description: "Training shoes",
    color: "green",
    size: 9,
    discontinued: false
});
coll.insert({
    _id: 7,
    item: "Shoes",
    description: "Formal shoes",
    color: "black",
    size: 8,
    discontinued: false
});
coll.insert({
    _id: 8,
    item: "Shoes",
    description: "Basketball shoes",
    color: "crimson red",
    size: 10,
    discontinued: false
});
coll.insert({
    _id: 9,
    item: "Shoes",
    description: "Designer shoes",
    color: "black",
    size: 7,
    discontinued: true
});
coll.insert(
    {_id: 10, item: "Phone", description: "Foldable phone", color: "gray", discontinued: false});
coll.insert({
    _id: 11,
    item: "Chocolate",
    description: "White chocolate bar",
    color: "white",
    discontinued: false
});

// A simple test to validate $search results based on a string (here color: red)
// This test is intended to include a positive test case for createSearchIndex instead of
// testing createSearchIndexes with single element array index. Reason behind that is
// createSearchIndex internally calls createSearchIndexes with single element array.
function createSearchIndexTest() {
    coll.createSearchIndex({name: "simpleIndex", definition: {"mappings": {"dynamic": true}}},
                           {blockUntilSearchIndexQueryable: true});
    const results =
        coll.aggregate(
                {$search: {"index": "simpleIndex", "text": {"path": "color", "query": "red"}}},
                {$sort: {_id: 1}})
            .toArray();

    const expectedResults = [
        {
            _id: 0,
            item: "Shoes",
            description: "Running shoes",
            color: "red",
            size: 10,
            discontinued: true
        },
        {
            _id: 4,
            item: "Dress",
            description: "Formal collection for work",
            color: "crimson red",
            discontinued: false
        },
        {
            _id: 8,
            item: "Shoes",
            description: "Basketball shoes",
            color: "crimson red",
            size: 10,
            discontinued: false
        }
    ]
    assert.eq(results.length, 3, results);

    assert.eq(results, expectedResults);
}

function createSearchIndexesTest() {
    coll.createSearchIndexes(
        [
            {
                name: "facetIndex",
                definition: {
                    "mappings": {
                        "dynamic": false,
                        "fields": {
                            "description": {"type": "string", "analyzer": "lucene.english"},
                            "color": {"type": "stringFacet"}
                        }
                    }
                }
            },
            {
                name: "sortIndex",
                definition: {
                    "mappings": {
                        "dynamic": false,
                        "fields": {"size": {"type": "number"}, "description": {"type": "string"}}
                    }
                }
            },
            {
                name: "highlightIndex",
                definition: {
                    "analyzer": "lucene.english",
                    "searchAnalyzer": "lucene.english",
                    "mappings": {"dynamic": true}
                }
            },
            {
                name: "paginationIndex",
                definition: {
                    "analyzer": "lucene.english",
                    "searchAnalyzer": "lucene.english",
                    "mappings": {"dynamic": true}
                }
            },
            {
                name: "returnStoredSourceIndex",
                definition: {
                    "mappings": {
                        "dynamic": false,
                        "fields": {"item": {"type": "string"}, "description": {"type": "string"}}
                    },
                    "storedSource": {"include": ["item", "discontinued"]}
                }
            }
        ],
        {blockUntilSearchIndexQueryable: true});

    // Query results are placed in buckets based on field "color"
    const facetResults =
        coll.aggregate({
                $searchMeta: {
                    "index": "facetIndex",
                    "facet": {
                        "operator": {"text": {"query": "shoes", "path": "description"}},
                        "facets":
                            {"colorFacet": {"type": "string", "path": "color", "numBuckets": 5}}
                    }
                }
            })
            .toArray();

    const facetResultBuckets = facetResults[0]["facet"]["colorFacet"]["buckets"];
    assert.eq(facetResultBuckets.length, 5);

    assert.eq(facetResultBuckets[0]["_id"], "black");
    assert.eq(facetResultBuckets[0]["count"], 2);

    assert.eq(facetResultBuckets[1]["_id"], "crimson red");
    assert.eq(facetResultBuckets[1]["count"], 1);

    // Sort based on Size field
    const sortResults = coll.aggregate({
                                $search: {
                                    "index": "sortIndex",
                                    "text": {"query": "shoes", "path": "description"},
                                    "sort": {"size": 1}
                                }
                            })
                            .toArray();

    assert.eq(sortResults[0]["size"], 7);
    assert.eq(sortResults[1]["size"], 8);
    assert.eq(sortResults[2]["size"], 9);
    assert.eq(sortResults[3]["size"], 10);

    const highlightResults =
        coll.aggregate(
                {
                    $search: {
                        "index": "highlightIndex",
                        "text": {"query": "chocolate", "path": "description"},
                        "highlight": {"path": "description"}
                    }
                },
                {
                    $project:
                        {"description": 1, "_id": 1, "highlights": {"$meta": "searchHighlights"}}
                })
            .toArray();

    // Highlight results from $searchMeta is an array of individual highlights for each document.
    // Individual highlights is an array of {"value": [string], "type": "hit" (or) "text" }
    // for a corresponding match

    const highlightResultFirstEntry = highlightResults[0]["highlights"][0]["texts"];
    assert.eq(highlightResultFirstEntry[0]["value"], "Assorted ");
    assert.eq(highlightResultFirstEntry[0]["type"], "text");
    assert.eq(highlightResultFirstEntry[1]["value"], "chocolate");
    assert.eq(highlightResultFirstEntry[1]["type"], "hit");
    assert.eq(highlightResultFirstEntry[2]["value"], " box");
    assert.eq(highlightResultFirstEntry[2]["type"], "text");

    const highlightResultSecondEntry = highlightResults[1]["highlights"][0]["texts"];
    assert.eq(highlightResultSecondEntry[0]["value"], "White ");
    assert.eq(highlightResultSecondEntry[0]["type"], "text");
    assert.eq(highlightResultSecondEntry[1]["value"], "chocolate");
    assert.eq(highlightResultSecondEntry[1]["type"], "hit");

    // Get the token to use with searchAfter
    const paginationResults =
        coll.aggregate({
                $search:
                    {"index": "highlightIndex", "text": {"query": "phone", "path": "description"}}
            },
                       {"$project": {"paginationToken": {"$meta": "searchSequenceToken"}}},
                       {"$sort": {_id: 1}},
                       {"$limit": 1})
            .toArray();

    const searchSequenceToken = paginationResults[0]["paginationToken"];
    const searchAfterResults = coll.aggregate({
                                       $search: {
                                           "index": "highlightIndex",
                                           "text": {"query": "phone", "path": "description"},
                                           "searchAfter": searchSequenceToken
                                       }
                                   })
                                   .toArray();

    // As there are 2 documents for the query and we've sorted and limited the original query result
    // to get the token, the subsequent result will have just the second entry for "phone"
    assert.eq(searchAfterResults.length, 1);
    assert.eq(searchAfterResults[0]["_id"], 10);

    const returnStoredSourceResults = coll.aggregate({
                                              $search: {
                                                  "index": "returnStoredSourceIndex",
                                                  "text": {"path": "item", "query": "Dress"},
                                                  "returnStoredSource": true
                                              }
                                          })
                                          .toArray();

    // The storedSource index option specifies the fields in the source document that mongot must
    // store. The returnStoredSource bool in the $search query indicates to mongot to retrieve only
    // those fields, rather than fetching full documents from the collection. This type of index +
    // query saves time/performance on the backend as the server is not required to lookup the full
    // document from IDs returned from mongot (the default behavior)."

    // Stored Source for just two fields as per returnStoredSourceIndex definition
    const returnStoredSourceexpectedResults = [
        {_id: 4, "item": "Dress", "discontinued": false},
        {_id: 3, "item": "Dress", "discontinued": true}
    ]
    assert.eq(returnStoredSourceResults, returnStoredSourceexpectedResults);
}

createSearchIndexTest();
createSearchIndexesTest();
