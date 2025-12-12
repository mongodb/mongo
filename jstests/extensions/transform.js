/**
 * Tests an extension transform stage.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const breadTypeOrderForDocs = ["sourdough", "rye", "whole wheat", "sourdough", "sourdough", "brioche"];
assert.commandWorked(coll.insertMany(breadTypeOrderForDocs.map((i) => ({breadType: i}))));

const runTestcase = (inputPipeline, expectedResults) => {
    results = coll.aggregate(buildPipeline(inputPipeline)).toArray();
    assertArrayEq({actual: results, expected: expectedResults, extraErrorMsg: tojson(results)});
};
const sortFieldsRemoveId = {
    $replaceRoot: {
        newRoot: {
            $arrayToObject: {
                $map: {
                    input: {$objectToArray: "$$ROOT"},
                    as: "loafField",
                    in: {
                        k: "$$loafField.k",
                        v: {
                            // Convert slices object to array, sort by breadType, renumber sequentially, remove _id
                            $arrayToObject: {
                                $reduce: {
                                    input: {
                                        $sortArray: {
                                            input: {$objectToArray: "$$loafField.v"},
                                            sortBy: {"v.breadType": 1},
                                        },
                                    },
                                    initialValue: [],
                                    in: {
                                        $concatArrays: [
                                            "$$value",
                                            [
                                                {
                                                    k: {$concat: ["slice", {$toString: {$size: "$$value"}}]},
                                                    v: {
                                                        $arrayToObject: {
                                                            $filter: {
                                                                input: {$objectToArray: "$$this.v"},
                                                                cond: {$ne: ["$$this.k", "_id"]},
                                                            },
                                                        },
                                                    },
                                                },
                                            ],
                                        ],
                                    },
                                },
                            },
                        },
                    },
                },
            },
        },
    },
};
const buildPipeline = (stages) => {
    const result = [];
    for (const stage of stages) {
        result.push(stage);
        if (stage.$loaf !== undefined) {
            result.push(sortFieldsRemoveId);
        }
    }
    return result;
};
const buildLoafStage = (numSlices) => {
    return {$loaf: {numSlices}};
};
const basicLoafStage = buildLoafStage(2);
const matchWithBasicLoafStage = [{$match: {breadType: "sourdough"}}, basicLoafStage];

// Transform stage must still be run against a collection.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: 1,
        pipeline: matchWithBasicLoafStage,
        cursor: {},
    }),
    ErrorCodes.InvalidNamespace,
);

// EOF transform stage.
let results = coll.aggregate([{$loaf: {numSlices: 0}}]).toArray();
assert.eq(results.length, 0, results);

// loaf as the first and only stage (set numSlices to collection length so it processes all collection documents).
{
    const expectedResults = [
        {
            fullLoaf: {
                slice0: {
                    breadType: "brioche",
                },
                slice1: {
                    breadType: "rye",
                },
                slice2: {
                    breadType: "sourdough",
                },
                slice3: {
                    breadType: "sourdough",
                },
                slice4: {
                    breadType: "sourdough",
                },
                slice5: {
                    breadType: "whole wheat",
                },
            },
        },
    ];
    const inputPipeline = [buildLoafStage(breadTypeOrderForDocs.length)];
    runTestcase(inputPipeline, expectedResults);
}

// Top-level transform stage with $match in pipeline.
{
    const expectedResults = [
        {
            fullLoaf: {
                slice0: {
                    breadType: "sourdough",
                },
                slice1: {
                    breadType: "sourdough",
                },
            },
        },
    ];
    runTestcase(matchWithBasicLoafStage, expectedResults);
}

// Check that a partial loaf is returned (per the getNext() logic for $loaf) when the
// number of docs returned by getNext() on the predecessor stage is less than the number of total
// slices that could be examined. Ex: there is only one matching entry for a breadType of "rye"
// but 2 total slices can be examined. We will hit eof after calling a getNext() for a second time
// on the predecessor stage and will therefore only be able to return a partial loaf with 1 slice
// instead of 2.
{
    const expectedResults = [
        {
            partialLoaf: {
                slice0: {
                    breadType: "rye",
                },
            },
        },
    ];
    const inputPipeline = [{$match: {breadType: "rye"}}, buildLoafStage(2)];
    runTestcase(inputPipeline, expectedResults);
}

// $loaf can appear consecutively in a pipeline.
{
    const expectedResults = [
        {
            partialLoaf: {
                slice0: {
                    fullLoaf: {
                        slice0: {
                            breadType: "sourdough",
                        },
                        slice1: {
                            breadType: "sourdough",
                        },
                    },
                },
            },
        },
    ];
    const inputPipeline = [{$match: {breadType: "sourdough"}}, buildLoafStage(2), buildLoafStage(2)];
    runTestcase(inputPipeline, expectedResults);
}

// Extension source stage $toast correctly provides input docs to $loaf.
{
    const expectedResults = [
        {
            fullLoaf: {
                slice0: {
                    slice: 0,
                    isBurnt: false,
                },
                slice1: {
                    slice: 1,
                    isBurnt: false,
                },
            },
        },
    ];
    results = db.runCommand({
        aggregate: "someCollection",
        pipeline: buildPipeline([{$toast: {temp: 300.0, numSlices: 4}}, buildLoafStage(2)]),
        cursor: {},
    }).cursor.firstBatch;
    assertArrayEq({actual: results, expected: expectedResults, extraErrorMsg: tojson(results)});
}

// Pipeline: $loaf -> (other server stages) -> $loaf
{
    const expectedResults = [
        {
            partialLoaf: {
                slice0: {
                    count: 1,
                },
                slice1: {
                    count: 1,
                },
                slice2: {
                    count: 3,
                },
                slice3: {
                    count: 1,
                },
            },
        },
    ];
    const inputPipeline = [
        buildLoafStage(breadTypeOrderForDocs.length),
        {$project: {slices: {$objectToArray: "$fullLoaf"}}}, // Convert object to array
        {$unwind: "$slices"}, // Now unwind the array
        {$replaceRoot: {newRoot: "$slices.v"}}, // Get the actual slice documents
        {$group: {_id: "$breadType", count: {$sum: 1}}}, // Sort by _id (breadType)
        {$sort: {_id: 1}},
        buildLoafStage(breadTypeOrderForDocs.length),
    ];
    runTestcase(inputPipeline, expectedResults);
}

// TODO SERVER-113930 Remove failure cases and enable success cases for $lookup and $unionWith.
// Transform stage in $lookup.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: collName,
        pipeline: [{$lookup: {from: collName, pipeline: [{$loaf: {numSlices: 2}}], as: "slices"}}],
        cursor: {},
    }),
    51047,
);
// results = coll.aggregate([{$lookup: {from: collName, pipeline: [{$loaf: {numSlices: 2}}], as: "slices"}}]).toArray();
// assert.gt(results.length, 0);

// Transform stage in $unionWith.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: collName,
        pipeline: [{$unionWith: {coll: collName, pipeline: [{$loaf: {numSlices: 2}}]}}],
        cursor: {},
    }),
    31441,
);
// results = coll.aggregate([{$unionWith: {coll: collName, pipeline: [{$loaf: {numSlices: 2}}]}}]).toArray();
// assert.gt(results.length, 0);

// Transform stage is not allowed in $facet.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: collName,
        pipeline: [{$facet: {slices: [{$loaf: {numSlices: 2}}]}}],
        cursor: {},
    }),
    40600,
);
