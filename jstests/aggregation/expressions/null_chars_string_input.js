/**
 * Tests how various aggregation expressions and stages that take strings and parameters respond to
 * string input containing null bytes.
 *
 * @tags: [
 *   # $listMqlEntities cannot be wrapped in a $facet stage.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
assert.commandWorked(coll.insert({_id: 1, foo: 1}));

const nullByteStrs = [
    // Starts with null chars.
    "\x00a",
    // Ends with null chars.
    "a\x00",
    // All null chars.
    "\x00",
    "\x00\x00\x00",
    // Null chars somewhere in the middle.
    "a\x00\x01\x08a",
    "a\x00\x02\x08b",
    "a\x00\x01\x18b",
    "a\x00\x01\x28c",
    "a\x00\x01\x03d\x00\xff\xff\xff\xff\x00\x08b",
];

function getStringUses(str) {
    return [str, `foo.${str}`];
}

function getFieldUses(str) {
    return [`$${str}`, `$foo.${str}`];
}

function getAllUses(str) {
    return [...getStringUses(str), ...getFieldUses(str)];
}

// Confirm that the JavaScript engine in the shell fails to construct the JS object because
// 'JavaScript property (name) contains a null char which is not allowed in BSON'.
function getShellErrorPipelines(nullStr) {
    return [
        [{$documents: [{[nullStr]: "foo"}], $match: {}}],
        [{$facet: {[nullStr]: [{$match: {}}]}}],
        [{$fill: {output: {[nullStr]: {value: "foo"}}}}],
        [{$fill: {sortBy: {[nullStr]: 1}, output: {[nullStr]: {value: "foo"}}}}],
        [{$group: {_id: "$foo", [nullStr]: {$sum: "$bar"}}}],
        [{$match: {[nullStr]: "foo"}}],
        [{$match: {$or: [{"foo": "bar"}, {[nullStr]: "baz"}]}}],
        [{
            $match:
                {$jsonSchema: {required: ["foo"], properties: {[nullStr]: {bsonType: "string"}}}}
        }],
        [{$merge: {into: "coll", on: "_id", let : {[nullStr]: "$foo"}}}],
        [{$project: {[nullStr]: 1}}],
        [{$project: {result: {$let: {vars: {[nullStr]: "$foo"}, in : "$$nullStr"}}}}],
        [{$replaceRoot: {newRoot: {[nullStr]: "$foo"}}}],
        [{$replaceWith: {[nullStr]: "$foo"}}],
        [{$set: {[nullStr]: "$foo"}}],
        [{$setWindowFields: {sortBy: {[nullStr]: 1}, output: {count: {$sum: 1}}}}],
        [{$setWindowFields: {output: {[nullStr]: {count: {$sum: 1}}}}}],
        [{$sort: {[nullStr]: 1}}],
        [{$unset: {[nullStr]: ""}}]
    ];
}

for (const nullStr of nullByteStrs) {
    for (const str of getAllUses(nullStr)) {
        for (const pipeline of getShellErrorPipelines(str)) {
            assert.throwsWithCode(() => coll.aggregate(pipeline), 16985);
        }
    }
}

// Certain expressions and stages are valid when passed a literal string that contains a null byte,
// but are invalid when the string is a reference to a field name.
function getFieldPathErrorPipelines(nullStr) {
    let pipelines = [
        [{$addFields: {field: nullStr}}],
        [{$addFields: {hashedVal: {$toHashedIndexKey: nullStr}}}],
        [{$set: {field: nullStr}}],
        [{$group: {_id: nullStr}}],
        [{$bucket: {groupBy: "$foo", boundaries: [nullStr, nullStr + "1"], default: "Other"}}],
        [{$bucket: {groupBy: "$foo", boundaries: [0, 5, 10], default: nullStr}}],
        [{$fill: {partitionBy: nullStr, sortBy: {foo: 1}, output: {out: {method: "linear"}}}}],
        [{$setWindowFields: {partitionBy: nullStr, output: {count: {$sum: 1}}}}],
    ];

    const nullStrComparisons = [
        {$eq: ["foo", nullStr]},
        {$ne: ["foo", nullStr]},
        {$gt: ["foo", nullStr]},
        {$gte: ["foo", nullStr]},
        {$lt: ["foo", nullStr]},
        {$lte: ["foo", nullStr]},
        {$in: ["foo", [nullStr]]}
    ];

    pipelines =
        pipelines.concat(nullStrComparisons.map(expr => [{$match: {$expr: {field: expr}}}]));

    // TODO SERVER-99206: Add testing for all expressions and modify the $listMqlEntities pipeline
    // below to confirm that every expression is covered.
    const expressionTests = [
        {$concat: [nullStr, "foo"]},
        {$ltrim: {input: nullStr}},
        {$max: ["foo", nullStr]},
        {$min: ["foo", nullStr]},
        {$rtrim: {input: nullStr}},
        {$substr: [nullStr, 0, 1]},
        {$substrBytes: [nullStr, 0, 1]},
        {$substrCP: [nullStr, 0, 1]},
        {$strcasecmp: [nullStr, "foo"]},
        {$trim: {input: nullStr}},
        {$toLower: nullStr},
        {$toString: nullStr},
        {$toUpper: nullStr},
        {$reduce: {input: [nullStr], initialValue: "", in : ""}},
        {$reduce: {input: ["foo"], initialValue: nullStr, in : ""}},
        {$reduce: {input: ["foo"], initialValue: "", in : nullStr}},
        {$regexMatch: {input: nullStr, regex: "foo"}},
        {$getField: nullStr},
    ];

    return pipelines.concat(expressionTests.map(operator => [{$project: {field: operator}}]));
}

// Confirm the behavior for all the pipelines that should succeed with null-byte literal strings and
// fail with field path expressions containing a null byte.
for (const nullStr of nullByteStrs) {
    for (const str of getStringUses(nullStr)) {
        for (const pipeline of getFieldPathErrorPipelines(str)) {
            assert.commandWorked(coll.runCommand('aggregate', {pipeline: pipeline, cursor: {}}));
        }
    }
    // When there is an embedded null byte in a field path, we expect error code 16411 in
    // particular.
    for (const field of getFieldUses(nullStr)) {
        for (const pipeline of getFieldPathErrorPipelines(field)) {
            assert.throwsWithCode(() => coll.aggregate(pipeline), 16411);
        }
    }
}

// Return expressions that should always fail when passed a string (literal or field name)
// containing a null byte.
function getErrorPipelines(nullStr) {
    return [
        {
            pipeline: [{$bucket: {groupBy: nullStr, boundaries: [0, 5, 10], default: "Other"}}],
            codes: [40202, 16411]
        },
        {
            pipeline: [{$bucketAuto: {groupBy: nullStr, buckets: 5, output: {count: {$sum: 1}}}}],
            codes: [40239, 16411]
        },
        {pipeline: [{$changeStream: {fullDocument: nullStr}}], codes: [ErrorCodes.BadValue]},
        {
            pipeline: [{$changeStream: {fullDocumentBeforeChange: nullStr}}],
            codes: [ErrorCodes.BadValue]
        },
        {pipeline: [{$count: nullStr}], codes: [40159, 40158]},
        {
            pipeline: [{$densify: {field: nullStr, range: {step: 1, bounds: "full"}}}],
            codes: [16411, 16410]
        },
        {
            pipeline: [{$densify: {field: "foo", range: {step: 1, bounds: nullStr}}}],
            codes: [5946802]
        },
        {
            pipeline: [{
                $densify: {
                    field: "foo",
                    partitionByFields: [nullStr],
                    range: {step: 1, bounds: "full"}
                }
            }],
            codes: [16411, 16410, 8993000]
        },
        {
            pipeline: [{$fill: {partitionByFields: [nullStr], output: {foo: {value: "bar"}}}}],
            codes: [16411, 16410]
        },
        {
            pipeline:
                [{$geoNear: {near: {type: "Point", coordinates: [0, 0]}, distanceField: nullStr}}],
            codes: [16411, 16410]
        },
        {
            pipeline: [{
                $geoNear: {
                    near: {type: "Point", coordinates: [0, 0]},
                    distanceField: "foo",
                    includeLocs: nullStr
                }
            }],
            codes: [16411, 16410]
        },
        {
            pipeline: [{
                $graphLookup: {
                    from: nullStr,
                    startWith: "$foo",
                    connectFromField: "parentId",
                    connectToField: "_id",
                    as: "results"
                }
            }],
            codes: [ErrorCodes.InvalidNamespace]
        },
        {
            pipeline: [{
                $graphLookup: {
                    from: "coll",
                    startWith: "$foo",
                    connectFromField: nullStr,
                    connectToField: "_id",
                    as: "results"
                }
            }],
            codes: [16411, 16410]
        },
        {
            pipeline: [{
                $graphLookup: {
                    from: "coll",
                    startWith: "$foo",
                    connectFromField: "parentId",
                    connectToField: nullStr,
                    as: "results"
                }
            }],
            codes: [16411, 16410]
        },
        {
            pipeline: [{
                $graphLookup: {
                    from: "coll",
                    startWith: "$foo",
                    connectFromField: "parentId",
                    connectToField: "_id",
                    as: nullStr
                }
            }],
            codes: [16411, 16410]
        },
        {
            pipeline: [{
                $graphLookup: {
                    from: "coll",
                    startWith: "$foo",
                    connectFromField: "parentId",
                    connectToField: "_id",
                    as: "results",
                    depthField: nullStr
                }
            }],
            codes: [16411, 16410]
        },
        {
            pipeline: [{
                $lookup: {
                    from: nullStr,
                    localField: "local",
                    foreignField: "foreign",
                    as: "result"
                }
            }],
            codes: [ErrorCodes.InvalidNamespace]
        },
        {
            pipeline: [{
                $lookup: {
                    from: "foo",
                    localField: nullStr,
                    foreignField: "foreign",
                    as: "result"
                }
            }],
            codes: [16411, 16410]
        },
        {
            pipeline: [{
                $lookup: {
                    from: "foo",
                    localField: "local",
                    foreignField: nullStr,
                    as: "result"
                }
            }],
            codes: [16411, 16410]
        },
        {
            pipeline: [{
                $lookup: {
                    from: "foo",
                    localField: "local",
                    foreignField: "foreign",
                    as: nullStr
                }
            }],
            codes: [16411, 16410]
        },
        {pipeline: [{$merge: {into: nullStr}}], codes: [ErrorCodes.InvalidNamespace]},
        {pipeline: [{$merge: {into: "coll", on: nullStr}}], codes: [16411, 16410]},
        {pipeline: [{$out: {db: nullStr, coll: "coll"}}], codes: [ErrorCodes.InvalidNamespace]},
        {pipeline: [{$out: {db: "db", coll: nullStr}}], codes: [ErrorCodes.InvalidNamespace]},
        {
            pipeline:
                [{$project: {field: {$setField: {field: nullStr, input: {}, value: "newField"}}}}],
            codes: [9534700, 16411]
        },
        {
            pipeline: [{$project: {field: {$unsetField: {field: nullStr, input: {}}}}}],
            codes: [9534700, 16411]
        },
        {
            pipeline: [{$project: {matches: {$regexMatch: {input: "$foo", regex: nullStr}}}}],
            codes: [51109, 16411]
        },
        {pipeline: [{$replaceRoot: {newRoot: nullStr}}], codes: [40228, 16411, 8105800]},
        {pipeline: [{$replaceWith: nullStr}], codes: [40228, 16411, 8105800]},
        {pipeline: [{$sortByCount: nullStr}], codes: [40148, 16411]},
        {pipeline: [{$unionWith: {coll: nullStr}}], codes: [ErrorCodes.InvalidNamespace]},
        {pipeline: [{$unwind: {path: nullStr}}], codes: [28818, 16419]},
        {pipeline: [{$unwind: {path: "$foo", includeArrayIndex: nullStr}}], codes: [16411, 28822]},
    ];
}

// Confirm the "error pipelines" always throw an exception.
for (const nullStr of nullByteStrs) {
    for (const strOrField of getAllUses(nullStr)) {
        for (const {pipeline, codes} of getErrorPipelines(strOrField)) {
            assert.throwsWithCode(() => coll.aggregate(pipeline), codes);
        }
    }
}

// Use $listMqlEntities to confirm that all aggregation stages have been tested for null byte input.
const aggStages = db.getSiblingDB("admin")
                      .aggregate([{$listMqlEntities: {entityType: "aggregationStages"}}])
                      .toArray()
                      .map(obj => obj.name);

// The following pipeline stages do not need to be tested for null byte input. Unless noted
// otherwise, assume these pipelines are skipped because they do not accept string input.
const skips = new Set([
    "$_addReshardingResumeId",
    "$_analyzeShardKeyReadWriteDistribution",
    "$_backupFile",
    "$_internalAllCollectionStats",
    "$_internalApplyOplogUpdate",
    "$_internalBoundedSort",
    "$_internalChangeStreamAddPostImage",
    "$_internalChangeStreamAddPreImage",
    "$_internalChangeStreamCheckInvalidate",
    "$_internalChangeStreamCheckResumability",
    "$_internalChangeStreamCheckTopologyChange",
    "$_internalChangeStreamHandleTopologyChange",
    "$_internalChangeStreamOplogMatch",
    "$_internalChangeStreamTransform",
    "$_internalChangeStreamUnwindTransaction",
    "$_internalComputeGeoNearDistance",
    "$_internalConvertBucketIndexStats",
    "$_internalDensify",
    "$_internalFindAndModifyImageLookup",
    "$_internalInhibitOptimization",
    "$_internalListCollections",
    "$_internalReshardingIterateTransaction",
    "$_internalReshardingOwnershipMatch",
    "$_internalSearchIdLookup",
    "$_internalSetWindowFields",
    "$_internalShardServerInfo",
    "$_internalShredDocuments",
    "$_internalSplitPipeline",
    "$_internalStreamingGroup",
    "$_internalUnpackBucket",  // Tested in timeseries_explicit_unpack_bucket.js.
    "$_unpackBucket",          // Tested in timeseries_explicit_unpack_bucket.js.
    "$backupCursor",
    "$backupCursorExtend",
    "$changeStreamSplitLargeEvent",
    "$collStats",
    "$currentOp",
    "$hoppingWindow",
    "$https",
    "$indexStats",
    "$limit",
    "$listCachedAndActiveUsers",
    "$listCatalog",
    "$listClusterCatalog",
    "$listLocalSessions",
    "$listMqlEntities",
    "$listSampledQueries",  // Tested in list_sampled_queries.js.
    "$listSearchIndexes",
    "$listSessions",
    "$mergeCursors",
    "$operationMetrics",
    "$planCacheStats",
    "$querySettings",
    "$queryStats",
    "$queue",
    "$rankFusion",
    "$redact",
    "$sample",
    "$score",
    "$scoreFusion",
    "$search",
    "$searchBeta",
    "$searchMeta",
    "$sessionWindow",
    "$setMetadata",
    "$setVariableFromSubPipeline",
    "$shardedDataDistribution",
    "$skip",
    "$tumblingWindow",
    "$validate",
    "$vectorSearch"
]);

const allPipelines = [
    ...getShellErrorPipelines(""),
    ...getFieldPathErrorPipelines(""),
    ...getErrorPipelines("").map(obj => obj.pipeline)
];
const testedStages =
    new Set(allPipelines.flatMap(pipeline => pipeline.map(obj => Object.keys(obj)[0])));

for (const aggStage of aggStages) {
    assert(testedStages.has(aggStage) || skips.has(aggStage),
           aggStage + " has not been tested for null bytes.");
}
