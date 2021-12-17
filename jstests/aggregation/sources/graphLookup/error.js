// In MongoDB 3.4, $graphLookup was introduced. In this file, we test the error cases.

load("jstests/aggregation/extras/utils.js");        // For "assertErrorCode".
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isSharded.

var local = db.local;
var foreign = db.foreign;

local.drop();
assert.commandWorked(local.insert({b: 0}));

foreign.drop();

// If the foreign collection is not implicitly sharded or the flag to allow $lookup/$graphLookup
// into a sharded collection is enabled, the $graphLookup pipelines should be allowed to execute.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
const canExecuteGraphLookup = !FixtureHelpers.isSharded(foreign) || isShardedLookupEnabled;

// Helper for asserting that appropriate error code is thrown depending on if the foreign collection
// in a $graphLookup is sharded and if it is allowed to be.
function assertFromCannotBeShardedOrError(pipeline, errorCode, msg) {
    if (canExecuteGraphLookup) {
        assertErrorCode(local, pipeline, errorCode, msg);
    } else {
        assertErrorCode(local, pipeline, 28769, "foreign collection cannot be sharded");
    }
}

var pipeline = {$graphLookup: 4};
assertErrorCode(local, pipeline, ErrorCodes.FailedToParse, "$graphLookup spec must be an object");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            maxDepth: "string"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40100, "maxDepth must be numeric");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            maxDepth: -1
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40101, "maxDepth must be nonnegative");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            maxDepth: 2.3
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40102, "maxDepth must be representable as a long long");

pipeline = {
        $graphLookup: {
            from: -1,
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output"
        }
    };
assertErrorCode(local, pipeline, ErrorCodes.FailedToParse, "from must be a string");

pipeline = {
        $graphLookup: {
            from: "",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output"
        }
    };
assertErrorCode(local, pipeline, ErrorCodes.InvalidNamespace, "from must be a valid namespace");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: 0
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40103, "as must be a string");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "$output"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 16410, "as cannot be a fieldPath");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: 0,
            as: "output"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40103, "connectFromField must be a string");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "$b",
            as: "output"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 16410, "connectFromField cannot be a fieldPath");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: 0,
            connectFromField: "b",
            as: "output"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40103, "connectToField must be a string");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "$a",
            connectFromField: "b",
            as: "output"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 16410, "connectToField cannot be a fieldPath");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            depthField: 0
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40103, "depthField must be a string");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            depthField: "$depth"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 16410, "depthField cannot be a fieldPath");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            restrictSearchWithMatch: "notamatch"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40185, "restrictSearchWithMatch must be an object");

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            notAField: "foo"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40104, "unknown argument");

pipeline = {
    $graphLookup: {from: "foreign", startWith: {$literal: 0}, connectFromField: "b", as: "output"}
};
assertFromCannotBeShardedOrError(pipeline, 40105, "connectToField was not specified");

pipeline = {
    $graphLookup: {from: "foreign", startWith: {$literal: 0}, connectToField: "a", as: "output"}
};
assertFromCannotBeShardedOrError(pipeline, 40105, "connectFromField was not specified");

pipeline = {
    $graphLookup: {from: "foreign", connectToField: "a", connectFromField: "b", as: "output"}
};
assertFromCannotBeShardedOrError(pipeline, 40105, "startWith was not specified");

pipeline = {
    $graphLookup:
        {from: "foreign", startWith: {$literal: 0}, connectToField: "a", connectFromField: "b"}
};
assertFromCannotBeShardedOrError(pipeline, 40105, "as was not specified");

pipeline = {
    $graphLookup:
        {startWith: {$literal: 0}, connectToField: "a", connectFromField: "b", as: "output"}
};
assertErrorCode(local, pipeline, ErrorCodes.FailedToParse, "from was not specified");

// restrictSearchWithMatch must be a valid match expression.
pipeline = {
        $graphLookup: {
            from: 'foreign',
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            restrictSearchWithMatch: {$not: {a: 1}}
        }
    };
if (canExecuteGraphLookup) {
    assert.throws(() => local.aggregate(pipeline), [], "unable to parse match expression");
} else {
    assertErrorCode(local, pipeline, 28769, "foreign collection cannot be sharded");
}

// $where and $text cannot be used inside $graphLookup.
pipeline = {
        $graphLookup: {
            from: 'foreign',
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            restrictSearchWithMatch: {$where: "3 > 2"}
        }
    };
if (canExecuteGraphLookup) {
    assert.throws(() => local.aggregate(pipeline), [], "cannot use $where inside $graphLookup");
} else {
    assertErrorCode(local, pipeline, 28769, "foreign collection cannot be sharded");
}

pipeline = {
        $graphLookup: {
            from: 'foreign',
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            restrictSearchWithMatch: {$text: {$search: "some text"}}
        }
    };
if (canExecuteGraphLookup) {
    assert.throws(() => local.aggregate(pipeline), [], "cannot use $text inside $graphLookup");
} else {
    assertErrorCode(local, pipeline, 28769, "foreign collection cannot be sharded");
}

pipeline = {
        $graphLookup: {
            from: 'foreign',
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            restrictSearchWithMatch: {
                x: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 100}}
            }
        }
    };
if (canExecuteGraphLookup) {
    assert.throws(() => local.aggregate(pipeline), [], "cannot use $near inside $graphLookup");
} else {
    assertErrorCode(local, pipeline, 28769, "foreign collection cannot be sharded");
}

pipeline = {
        $graphLookup: {
            from: 'foreign',
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            restrictSearchWithMatch: {
                $and: [{
                    x: {
                        $near: {
                            $geometry: {type: 'Point', coordinates: [0, 0]},
                            $maxDistance: 100
                        }
                    }
                }]
            }
        }
    };
if (canExecuteGraphLookup) {
    assert.throws(
        () => local.aggregate(pipeline), [], "cannot use $near inside $graphLookup at any depth");
} else {
    assertErrorCode(local, pipeline, 28769, "foreign collection cannot be sharded");
}

// let foreign = db.foreign;
foreign.drop();
assert.commandWorked(foreign.insert({a: 0, x: 0}));

// Test a restrictSearchWithMatch expression that fails to parse.
pipeline = {
        $graphLookup: {
            from: 'foreign',
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            restrictSearchWithMatch: {$expr: {$eq: ["$x", "$$unbound"]}}
        }
    };
if (canExecuteGraphLookup) {
    assert.throws(() => local.aggregate(pipeline), [], "cannot use $expr with unbound variable");
} else {
    assertErrorCode(local, pipeline, 28769, "foreign collection cannot be sharded");
}

// Test a restrictSearchWithMatchExpression that throws at runtime.
pipeline = {
        $graphLookup: {
            from: 'foreign',
            startWith: {$literal: 0},
            connectToField: "a",
            connectFromField: "b",
            as: "output",
            restrictSearchWithMatch: {$expr: {$divide: [1, "$x"]}}
        }
    };
if (canExecuteGraphLookup) {
    assertErrorCode(local, pipeline, [16608, ErrorCodes.BadValue], "division by zero in $expr");
} else {
    assertErrorCode(local, pipeline, 28769, "foreign collection cannot be sharded");
}

// $graphLookup can only consume at most 100MB of memory.
foreign.drop();

// Here, the visited set exceeds 100MB.
var bulk = foreign.initializeUnorderedBulkOp();

var initial = [];
for (var i = 0; i < 8; i++) {
    var obj = {_id: i};

    obj['longString'] = new Array(14 * 1024 * 1024).join('x');
    initial.push(i);
    bulk.insert(obj);
}
assert.commandWorked(bulk.execute());

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: initial},
            connectToField: "_id",
            connectFromField: "notimportant",
            as: "graph"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40099, "maximum memory usage reached");

// Here, the visited set should grow to approximately 90 MB, and the frontier should push memory
// usage over 100MB.
foreign.drop();

var bulk = foreign.initializeUnorderedBulkOp();
for (var i = 0; i < 14; i++) {
    var obj = {from: 0, to: 1};
    obj['s'] = new Array(7 * 1024 * 1024).join(' ');
    bulk.insert(obj);
}
assert.commandWorked(bulk.execute());

pipeline = {
        $graphLookup: {
            from: "foreign",
            startWith: {$literal: 0},
            connectToField: "from",
            connectFromField: "s",
            as: "out"
        }
    };
assertFromCannotBeShardedOrError(pipeline, 40099, "maximum memory usage reached");

// Here, we test that the cache keeps memory usage under 100MB, and does not cause an error.
foreign.drop();

var bulk = foreign.initializeUnorderedBulkOp();
for (var i = 0; i < 13; i++) {
    var obj = {from: 0, to: 1};
    obj['s'] = new Array(7 * 1024 * 1024).join(' ');
    bulk.insert(obj);
}
assert.commandWorked(bulk.execute());

if (canExecuteGraphLookup) {
    var res = local
                    .aggregate({
                        $graphLookup: {
                            from: "foreign",
                            startWith: {$literal: 0},
                            connectToField: "from",
                            connectFromField: "to",
                            as: "out"
                        }
                    },
                                {$unwind: {path: "$out"}})
                    .toArray();

    assert.eq(res.length, 13);
}
}());
