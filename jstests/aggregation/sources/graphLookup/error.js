// In MongoDB 3.4, $graphLookup was introduced. In this file, we test the error cases.
//
// @tags: [
//   not_allowed_with_signed_security_token,
//   requires_fcv_82,
// ]
import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getKnob(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setKnob(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

let local = db.local;
let foreign = db.foreign;

local.drop();
assert.commandWorked(local.insert({b: 0}));

foreign.drop();

let pipeline = {$graphLookup: 4};
assertErrorCode(local, pipeline, ErrorCodes.FailedToParse, "$graphLookup spec must be an object");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        maxDepth: "string",
    },
};
assertErrorCode(local, pipeline, 40100, "maxDepth must be numeric");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        maxDepth: -1,
    },
};
assertErrorCode(local, pipeline, 40101, "maxDepth must be nonnegative");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        maxDepth: 2.3,
    },
};
assertErrorCode(local, pipeline, 40102, "maxDepth must be representable as a long long");

pipeline = {
    $graphLookup: {
        from: -1,
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
    },
};
assertErrorCode(local, pipeline, ErrorCodes.FailedToParse, "from must be a string");

pipeline = {
    $graphLookup: {
        from: "",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
    },
};
assertErrorCode(local, pipeline, ErrorCodes.InvalidNamespace, "from must be a valid namespace");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: 0,
    },
};
assertErrorCode(local, pipeline, 40103, "as must be a string");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "$output",
    },
};
assertErrorCode(local, pipeline, 16410, "as cannot be a fieldPath");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: 0,
        as: "output",
    },
};
assertErrorCode(local, pipeline, 40103, "connectFromField must be a string");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "$b",
        as: "output",
    },
};
assertErrorCode(local, pipeline, 16410, "connectFromField cannot be a fieldPath");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: 0,
        connectFromField: "b",
        as: "output",
    },
};
assertErrorCode(local, pipeline, 40103, "connectToField must be a string");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "$a",
        connectFromField: "b",
        as: "output",
    },
};
assertErrorCode(local, pipeline, 16410, "connectToField cannot be a fieldPath");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        depthField: 0,
    },
};
assertErrorCode(local, pipeline, 40103, "depthField must be a string");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        depthField: "$depth",
    },
};
assertErrorCode(local, pipeline, 16410, "depthField cannot be a fieldPath");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        restrictSearchWithMatch: "notamatch",
    },
};
assertErrorCode(local, pipeline, 40185, "restrictSearchWithMatch must be an object");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        notAField: "foo",
    },
};
assertErrorCode(local, pipeline, 40104, "unknown argument");

pipeline = {
    $graphLookup: {from: "foreign", startWith: {$literal: 0}, connectFromField: "b", as: "output"},
};
assertErrorCode(local, pipeline, 40105, "connectToField was not specified");

pipeline = {
    $graphLookup: {from: "foreign", startWith: {$literal: 0}, connectToField: "a", as: "output"},
};
assertErrorCode(local, pipeline, 40105, "connectFromField was not specified");

pipeline = {
    $graphLookup: {from: "foreign", connectToField: "a", connectFromField: "b", as: "output"},
};
assertErrorCode(local, pipeline, 40105, "startWith was not specified");

pipeline = {
    $graphLookup: {from: "foreign", startWith: {$literal: 0}, connectToField: "a", connectFromField: "b"},
};
assertErrorCode(local, pipeline, 40105, "as was not specified");

pipeline = {
    $graphLookup: {startWith: {$literal: 0}, connectToField: "a", connectFromField: "b", as: "output"},
};
assertErrorCode(local, pipeline, ErrorCodes.FailedToParse, "from was not specified");

// restrictSearchWithMatch must be a valid match expression.
pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        restrictSearchWithMatch: {$not: {a: 1}},
    },
};
assert.throws(() => local.aggregate(pipeline), [], "unable to parse match expression");

// $where and $text cannot be used inside $graphLookup.
pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        restrictSearchWithMatch: {$where: "3 > 2"},
    },
};
assert.throws(() => local.aggregate(pipeline), [], "cannot use $where inside $graphLookup");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        restrictSearchWithMatch: {$text: {$search: "some text"}},
    },
};
assert.throws(() => local.aggregate(pipeline), [], "cannot use $text inside $graphLookup");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        restrictSearchWithMatch: {
            x: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}, $maxDistance: 100}},
        },
    },
};
assert.throws(() => local.aggregate(pipeline), [], "cannot use $near inside $graphLookup");

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        restrictSearchWithMatch: {
            $and: [
                {
                    x: {
                        $near: {
                            $geometry: {type: "Point", coordinates: [0, 0]},
                            $maxDistance: 100,
                        },
                    },
                },
            ],
        },
    },
};
assert.throws(() => local.aggregate(pipeline), [], "cannot use $near inside $graphLookup at any depth");

// let foreign = db.foreign;
foreign.drop();
assert.commandWorked(foreign.insert({a: 0, x: 0}));

// Test a restrictSearchWithMatch expression that fails to parse.
pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        restrictSearchWithMatch: {$expr: {$eq: ["$x", "$$unbound"]}},
    },
};
assert.throws(() => local.aggregate(pipeline), [], "cannot use $expr with unbound variable");

// Test a restrictSearchWithMatchExpression that throws at runtime.
pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "a",
        connectFromField: "b",
        as: "output",
        restrictSearchWithMatch: {$expr: {$divide: [1, "$x"]}},
    },
};
assertErrorCode(local, pipeline, [16608, ErrorCodes.BadValue], "division by zero in $expr");

// $graphLookup can only consume at most 100MB of memory without spilling.
foreign.drop();

const string7KB = " ".repeat(7 * 1024);
const string14KB = string7KB + string7KB;

// Set memory limit to 100 KB to avoid consuming too much memory for the test.
const memoryLimitKnob = "internalDocumentSourceGraphLookupMaxMemoryBytes";
const originalMemoryLimitKnobValues = getKnob(memoryLimitKnob);
setKnob(memoryLimitKnob, 100 * 1024);

// Here, the visited set exceeds 100 KB.
let initial = [];
for (let i = 0; i < 8; i++) {
    let obj = {_id: i};
    obj["longString"] = string14KB;
    initial.push(i);
    assert.commandWorked(foreign.insertOne(obj));
}

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: initial},
        connectToField: "_id",
        connectFromField: "notimportant",
        as: "graph",
    },
};
assertErrorCode(
    local,
    pipeline,
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    "Exceeded memory limit and can't spill to disk",
    {allowDiskUse: false},
);

// Here, the visited set should grow to approximately 90 KB, and the queue should push memory
// usage over 100KB.
foreign.drop();

for (let i = 0; i < 14; i++) {
    let obj = {from: 0, to: 1};
    obj["s"] = string7KB;
    assert.commandWorked(foreign.insertOne(obj));
}

pipeline = {
    $graphLookup: {
        from: "foreign",
        startWith: {$literal: 0},
        connectToField: "from",
        connectFromField: "s",
        as: "out",
    },
};
assertErrorCode(
    local,
    pipeline,
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    "Exceeded memory limit and can't spill to disk",
    {allowDiskUse: false},
);

// Here, we test that the cache keeps memory usage under 100KB, and does not cause an error.
foreign.drop();
for (let i = 0; i < 13; i++) {
    let obj = {from: 0, to: 1};
    obj["s"] = string7KB;
    assert.commandWorked(foreign.insertOne(obj));
}

let res = local
    .aggregate(
        {
            $graphLookup: {
                from: "foreign",
                startWith: {$literal: 0},
                connectToField: "from",
                connectFromField: "to",
                as: "out",
            },
        },
        {$unwind: {path: "$out"}},
    )
    .toArray();

assert.eq(res.length, 13);

setKnob(memoryLimitKnob, originalMemoryLimitKnobValues);
