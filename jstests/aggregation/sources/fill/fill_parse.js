/**
 * Test the syntax of $fill.
 * @tags: [
 *   requires_fcv_52,
 *   # We're testing the explain plan, not the query results, so the facet passthrough would fail.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */

(function() {
load("jstests/libs/fixture_helpers.js");
load("jstests/libs/feature_flag_util.js");    // For isEnabled.
load("jstests/aggregation/extras/utils.js");  // For anyEq and desugarSingleStageAggregation.

const coll = db[jsTestName()];
coll.drop();

function buildAndRunCommand(stage) {
    return db.runCommand({aggregate: coll.getName(), pipeline: [stage], cursor: {}});
}

// Fail if not an object.
assert.commandFailedWithCode(buildAndRunCommand({$fill: "test"}), ErrorCodes.FailedToParse);

// Fail if 'output' is missing.
assert.commandFailedWithCode(buildAndRunCommand({$fill: {}}), 40414);

// Fail if 'output' is present but empty.
assert.commandFailedWithCode(buildAndRunCommand({$fill: {output: {}}}), 6050203);

// Fail on invalid method.
assert.commandFailedWithCode(buildAndRunCommand({$fill: {output: {test: {method: "random"}}}}),
                             6050202);

// Fail on invalid fill specification.
assert.commandFailedWithCode(buildAndRunCommand({$fill: {output: {test: "random"}}}), 6050200);
assert.commandFailedWithCode(
    buildAndRunCommand({$fill: {output: {test: {method: "locf", second: "locf"}}}}), 40415);

// Fail if both 'partitionBy' and 'partitionByFields' are specified.
assert.commandFailedWithCode(buildAndRunCommand({
                                 $fill: {
                                     output: {test: {method: "locf"}},
                                     partitionBy: {test: "$test"},
                                     partitionByFields: ["$test"]
                                 }
                             }),
                             6050204);

// Fail if linearFill does not receive a sortBy field.
assert.commandFailedWithCode(
    buildAndRunCommand({
        $fill:
            {output: {test: {method: "linear"}}, partitionBy: {part: "$part", partTwo: "$partTwo"}}
    }),
    605001);

// Test that we desugar correctly.
// Format is [[$fill spec], [Desugared pipeline], [field list that contains UUIDs]]
// Not all test cases have a spec that generates UUIDs, the third array will be empty for those
// tests.
let testCases = [
    [
        {$fill: {output: {val: {method: "locf"}}}},
        [{"$_internalSetWindowFields": {"output": {"val": {"$locf": "$val"}}}}],
        []
    ],  // 0
    [
        {$fill: {sortBy: {key: 1}, output: {val: {method: "linear"}}}},
        [
            {"$sort": {"sortKey": {"key": 1}}},
            {
                "$_internalSetWindowFields":
                    {"sortBy": {"key": 1}, "output": {"val": {"$linearFill": "$val"}}}
            }
        ],
        []
    ],  // 1
    [
        {$fill: {output: {val: {value: 5}}}},
        [{"$addFields": {"val": {$ifNull: ["$val", {"$const": 5}]}}}],
        []
    ],  // 2
    [
        {$fill: {output: {val: {value: "$test"}}}},
        [{"$addFields": {"val": {$ifNull: ["$val", "$test"]}}}],
        []
    ],  // 3
    [
        {$fill: {output: {val: {value: "$test"}, second: {method: "locf"}}}},
        [
            {"$_internalSetWindowFields": {"output": {"second": {"$locf": "$second"}}}},
            {"$addFields": {"val": {$ifNull: ["$val", "$test"]}}}
        ],
        []
    ],  // 4
    [
        {
            $fill: {
                output: {
                    val: {value: "$test"},
                    second: {method: "locf"},
                    third: {value: {$add: ["$val", "$second"]}},
                    fourth: {method: "locf"}
                }
            }
        },
        [
            {
                "$_internalSetWindowFields":
                    {"output": {"second": {"$locf": "$second"}, "fourth": {"$locf": "$fourth"}}}
            },
            {
                "$addFields": {
                    "val": {$ifNull: ["$val", "$test"]},
                    "third": {$ifNull: ["$third", {"$add": ["$val", "$second"]}]}
                }
            }
        ],
        []
    ],  // 5
    [
        {$fill: {output: {val: {method: "locf"}}, partitionByFields: ["part", "partTwo"]}},
        [
            {"$addFields": {"UUIDPLACEHOLDER": {"part": "$part", "partTwo": "$partTwo"}}},
            {"$sort": {"sortKey": {"UUIDPLACEHOLDER": 1}}},
            {
                "$_internalSetWindowFields":
                    {"partitionBy": "$UUIDPLACEHOLDER", "output": {"val": {"$locf": "$val"}}}
            },
            {"$project": {"UUIDPLACEHOLDER": false, "_id": true}}
        ],
        [
            [0, "$addFields", true],
            [1, "$sort", "sortKey", true],
            [2, "$_internalSetWindowFields", "partitionBy", false],
            [3, "$project", true]
        ]
    ],  // 6
    [
        {
            $fill:
                {output: {val: {method: "locf"}}, partitionBy: {part: "$part", partTwo: "$partTwo"}}
        },
        [
            {"$addFields": {"UUIDPLACEHOLDER": {"part": "$part", "partTwo": "$partTwo"}}},
            {"$sort": {"sortKey": {"UUIDPLACEHOLDER": 1}}},
            {
                "$_internalSetWindowFields":
                    {"partitionBy": "$UUIDPLACEHOLDER", "output": {"val": {"$locf": "$val"}}}
            },
            {"$project": {"UUIDPLACEHOLDER": false, "_id": true}}
        ],
        [
            [0, "$addFields", true],
            [1, "$sort", "sortKey", true],
            [2, "$_internalSetWindowFields", "partitionBy", false],
            [3, "$project", true]
        ]
    ],  // 7
    [
        {
            $fill: {
                output: {val: {method: "locf"}},
                sortBy: {key: 1},
                partitionBy: {part: "$part", partTwo: "$partTwo"}
            }
        },
        [
            {"$addFields": {"UUIDPLACEHOLDER": {"part": "$part", "partTwo": "$partTwo"}}},
            {"$sort": {"sortKey": {"UUIDPLACEHOLDER": 1, "key": 1}}},
            {
                "$_internalSetWindowFields": {
                    "partitionBy": "$UUIDPLACEHOLDER",
                    "sortBy": {"key": 1},
                    "output": {"val": {"$locf": "$val"}}
                }
            },
            {"$project": {"UUIDPLACEHOLDER": false, "_id": true}}
        ],
        [
            [0, "$addFields", true],
            [1, "$sort", "sortKey", true],
            [2, "$_internalSetWindowFields", "partitionBy", false],
            [3, "$project", true]
        ]
    ],  // 8
    [
        {
            $fill: {
                output: {val: {method: "locf"}, second: {value: 7}},
                sortBy: {key: 1},
                partitionBy: {part: "$part", partTwo: "$partTwo"}
            }
        },
        [
            {"$addFields": {"UUIDPLACEHOLDER": {"part": "$part", "partTwo": "$partTwo"}}},
            {"$sort": {"sortKey": {"UUIDPLACEHOLDER": 1, "key": 1}}},
            {
                "$_internalSetWindowFields": {
                    "partitionBy": "$UUIDPLACEHOLDER",
                    "sortBy": {"key": 1},
                    "output": {"val": {"$locf": "$val"}}
                }
            },
            {"$project": {"UUIDPLACEHOLDER": false, "_id": true}},
            {"$addFields": {"second": {$ifNull: ["$second", {"$const": 7}]}}}
        ],
        [
            [0, "$addFields", true],
            [1, "$sort", "sortKey", true],
            [2, "$_internalSetWindowFields", "partitionBy", false],
            [3, "$project", true]
        ],

    ],  // 9
];

function modifyObjectAtPath(orig, path) {
    if (typeof (path[0]) == "boolean") {
        // The first key in the object needs to be replaced.
        if (path[0]) {
            const firstKey = Object.keys(orig)[0];
            const val = orig[firstKey];
            delete orig[firstKey];
            orig["UUIDPLACEHOLDER"] = val;
        } else {
            // Orig is a string. If the first character is a '$', keep it.
            if (orig[0] === '$') {
                return "$UUIDPLACEHOLDER";
            }
            return "UUIDPLACEHOLDER";
        }
    } else if (typeof (path[0]) == "number" || typeof (path[0]) == "string") {
        // Orig is an array. Operate on an element of the array.
        orig[path[0]] = modifyObjectAtPath(orig[path[0]], path.slice(1));
    } else {
        // Sanity guard.
        assert(false, "Unexpected type in path " + typeof (path[0]) + "\n" + tojson(path[0]));
    }

    return orig;
}

for (let i = 0; i < testCases.length; i++) {
    let result = desugarSingleStageAggregation(db, coll, testCases[i][0]);
    // $setWindowFields generates random fieldnames. Use the paths in the test case to
    // replace the UUID with "UUIDPLACEHOLDER".
    if (testCases[i][2].length != 0) {
        for (let pathNum = 0; pathNum < testCases[i][2].length; pathNum++) {
            result = modifyObjectAtPath(result, testCases[i][2][pathNum]);
        }
    }

    assert(anyEq(result, testCases[i][1], false, null, "UUIDPLACEHOLDER"),
           "Test case " + i + " failed.\n" +
               "Expected:\n" + tojson(testCases[i][1]) + "\nGot:\n" + tojson(result));
}
})();
