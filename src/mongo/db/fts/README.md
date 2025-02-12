# Full Text Search (FTS) Indexing

MongoDB has had support for creating 'text' indexes for a long time (since at least 2.4). More
recently, this support has been deprioritized in favor of Atlas Search and the $search aggregation
stage.

Please refer to our documentation for some more detailed examples of using the feature.
Here we will go into the implementation a bit.

An FTS index in MongoDB is implemented as a multikey, sparse B-Tree in terms of the actual data
structure. As an example, consider this index:

```js
db.testCollection.createIndex({
  active: 1,
  subject: "text",
  content: "text",
  _id: 1,
});
```

This is demonstrating/highlighting that these indexes can be compound with other fields (text or
non-text) also.

If I were to insert this document:

```js
{
    _id: ObjectId("53470c2d30ceb30930f96884"),
    active: true,
    subject: "coffee",
    content: "what goes well"
}
```

Then the indexing code would end up with mulitple index keys (hence multikey) like:

```js
{
    active: true,
    _fts: "coffee",
    _ftsx: 1,
    _id: ObjectId("53470c2d30ceb30930f96884"),
}
{
    active: true,
    _fts: "goes",
    _ftsx: 0.666,
    _id: ObjectId("53470c2d30ceb30930f96884"),
}
{
    active: true,
    _fts: "well",
    _ftsx: 0.666,
    _id: ObjectId("53470c2d30ceb30930f96884"),
}
```

Note:

- 'stop' words (like those listed in `stop_words_english.txt`) such as 'what' are omitted. The
  thinking is that these don't provide a lot of value to search on, and are not worth taking up
  space in the index.
- the score is calculated according to some fairly complex logic, involving the relative frequency
  of each term. Generally, rarer (more unique) terms get higher scores.
- The index spec has two 'text' terms, but they are consolidated into one part of the index. Any
  number of 'text' index components can occur next to each other, and will always result in a '\_fts'
  component for the term and an '\_ftsx' component for the score.
- The scores are computed **with only one document considered at a time**. This means that if a
  particular token is quite rare within a single document, it'll get a high score even if it is
  otherwise quite common in the broader dataset. This is one notable benefit of the Atlas Search
  product ($search aggregation stage) which uses a lucene-powered "inverted index" to track overall
  term frequencies for the broader dataset.

As noted before, this is also sparse so that if you insert a document with no text, or with only
stop words, it won't get any index keys.

When searching, for example with the following query, we will scan the index much like a normal
index scan.

```js
db.example.find({active: true, $text: {$search: "goes well"}}).sort({{score: {meta: 'textScore'}});
```

This query is a logical OR of 'goes well', both of which are found in our example document. An
example query plan is shown below:

```js
{
        "explainVersion" : "1",
        "queryPlanner" : {
                "namespace" : "test.foo",
                "parsedQuery" : {
                        "$and" : [
                                {
                                        "active" : {
                                                "$eq" : true
                                        }
                                },
                                {
                                        "$text" : {
                                                "$search" : "ghost bird",
                                                "$language" : "english",
                                                "$caseSensitive" : false,
                                                "$diacriticSensitive" : false
                                        }
                                }
                        ]
                },
                "indexFilterSet" : false,
                "queryHash" : "3825BBED",
                "planCacheShapeHash" : "3825BBED",
                "planCacheKey" : "76FE542B",
                "optimizationTimeMillis" : 1,
                "maxIndexedOrSolutionsReached" : false,
                "maxIndexedAndSolutionsReached" : false,
                "maxScansToExplodeReached" : false,
                "prunedSimilarIndexes" : false,
                "winningPlan" : {
                        "isCached" : false,
                        "stage" : "SORT",
                        "sortPattern" : {
                                "$computed0" : {
                                        "$meta" : "textScore"
                                }
                        },
                        "memLimit" : 104857600,
                        "type" : "default",
                        "inputStage" : {
                                "stage" : "TEXT_MATCH",
                                "indexPrefix" : {
                                        "active" : true
                                },
                                "indexName" : "active_1_subject_text_content_text__id_1",
                                "parsedTextQuery" : {
                                        "terms" : [
                                                "bird",
                                                "ghost"
                                        ],
                                        "negatedTerms" : [ ],
                                        "phrases" : [ ],
                                        "negatedPhrases" : [ ]
                                },
                                "textIndexVersion" : 3,
                                "inputStage" : {
                                        "stage" : "TEXT_OR",
                                        "inputStages" : [
                                                {
                                                        "stage" : "IXSCAN",
                                                        "keyPattern" : {
                                                                "active" : 1,
                                                                "_fts" : "text",
                                                                "_ftsx" : 1,
                                                                "_id" : 1
                                                        },
                                                        "indexName" : "active_1_subject_text_content_text__id_1",
                                                        "isMultiKey" : true,
                                                        "isUnique" : false,
                                                        "isSparse" : false,
                                                        "isPartial" : false,
                                                        "indexVersion" : 2,
                                                        "direction" : "backward",
                                                        "indexBounds" : {

                                                        }
                                                },
                                                {
                                                        "stage" : "IXSCAN",
                                                        "keyPattern" : {
                                                                "active" : 1,
                                                                "_fts" : "text",
                                                                "_ftsx" : 1,
                                                                "_id" : 1
                                                        },
                                                        "indexName" : "active_1_subject_text_content_text__id_1",
                                                        "isMultiKey" : true,
                                                        "isUnique" : false,
                                                        "isSparse" : false,
                                                        "isPartial" : false,
                                                        "indexVersion" : 2,
                                                        "direction" : "backward",
                                                        "indexBounds" : {

                                                        }
                                                }
                                        ]
                                }
                        }
                },
                "rejectedPlans" : [ ]
        },
        "queryShapeHash" : "BEDDF2118C25D7CAF332A16919AA1DBA86EE292450772984D8AB598A445B885D",
        "command" : {
                "find" : "foo",
                "filter" : {
                        "active" : true,
                        "$text" : {
                                "$search" : "ghost bird"
                        }
                },
                "sort" : {
                        "score" : {
                                "$meta" : "textScore"
                        }
                },
                "$db" : "test"
        },
        "serverInfo" : {
                "host" : "ip-10-128-3-64",
                "port" : 27017,
                "version" : "8.1.0-alpha",
                "gitVersion" : "nogitversion"
        },
        "serverParameters" : {
                "internalQueryFacetBufferSizeBytes" : 104857600,
                "internalQueryFacetMaxOutputDocSizeBytes" : 104857600,
                "internalLookupStageIntermediateDocumentMaxSizeBytes" : 104857600,
                "internalDocumentSourceGroupMaxMemoryBytes" : 104857600,
                "internalQueryMaxBlockingSortMemoryUsageBytes" : 104857600,
                "internalQueryProhibitBlockingMergeOnMongoS" : 0,
                "internalQueryMaxAddToSetBytes" : 104857600,
                "internalDocumentSourceSetWindowFieldsMaxMemoryBytes" : 104857600,
                "internalQueryFrameworkControl" : "trySbeRestricted",
                "internalQueryPlannerIgnoreIndexWithCollationForRegex" : 1
        },
        "ok" : 1
}
```

Here note that the IXSCAN will de-duplicate by RecordId (as any normal index scan of a multikey
index will), and then the "TEXT_MATCH" stage will use the '\_ftsx' entries to apply a score to the
document. Finally, we sort by that score (this is not the default, so notice that the query had to
explicitly request this).

Those are the basics. Some final notes:

- users can specify weights to boost/customize the scores for different fields (for example giving a
  higher score to a 'title' than to a big blob of text.
- You can do a wildcard text index which indexes all text it can find in the document.

In terms of the code, you can find some of the key details inside:

- [FTSElementIterator](./fts_element_iterator.h) includes the logic for extracting the text values
  out of a document.
- `FTSIndexFormat::getKeys()` houses the logic for extracting the tokens and assigning scores and
  generating index keys.
