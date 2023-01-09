/**
 * Tests that language features introduced in version 5.2 are included in API Version 1.
 *
 * The test runs commands that are not allowed with security token: top.
 * @tags: [
 *   not_allowed_with_security_token,
 *   requires_fcv_60,
 *   uses_api_parameters,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/api_version_helpers.js");  // For 'APIVersionHelpers'.

const collName = "api_version_new_52_language_features";
const viewName = collName + "_view";
const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert({a: 1, arr: [2, 1, 4]}));

const stablePipelines = [
    [{$group: {_id: "$a", out: {$topN: {output: "$a", n: 2, sortBy: {a: 1}}}}}],
    [{$group: {_id: "$a", out: {$top: {output: "$a", sortBy: {a: 1}}}}}],
    [{$group: {_id: "$a", out: {$bottomN: {output: "$a", n: 2, sortBy: {a: 1}}}}}],
    [{$group: {_id: "$a", out: {$bottom: {output: "$a", sortBy: {a: 1}}}}}],
    [{$group: {_id: "$a", out: {$minN: {input: "$a", n: 2}}}}],
    [{$group: {_id: "$a", out: {$maxN: {input: "$a", n: 2}}}}],
    [{$group: {_id: "$a", out: {$firstN: {input: "$a", n: 2}}}}],
    [{$group: {_id: "$a", out: {$lastN: {input: "$a", n: 2}}}}],
    [{$set: {x: {$firstN: {input: "$arr", n: 2}}}}],
    [{$set: {x: {$lastN: {input: "$arr", n: 2}}}}],
    [{$set: {x: {$minN: {input: "$arr", n: 2}}}}],
    [{$set: {x: {$maxN: {input: "$arr", n: 2}}}}],
    [{$set: {x: {$sortArray: {input: "$arr", sortBy: 1}}}}],
    [{
        $setWindowFields: {
            partitionBy: "$a",
            sortBy: {a: 1},
            output: {out: {$topN: {output: "$a", n: 2, sortBy: {a: 1}}}}
        }
    }],
    [{
        $setWindowFields: {
            partitionBy: "$a",
            sortBy: {a: 1},
            output: {out: {$top: {output: "$a", sortBy: {a: 1}}}}
        }
    }],
    [{
        $setWindowFields: {
            partitionBy: "$a",
            sortBy: {a: 1},
            output: {out: {$bottomN: {output: "$a", n: 2, sortBy: {a: 1}}}}
        }
    }],
    [{
        $setWindowFields: {
            partitionBy: "$a",
            sortBy: {a: 1},
            output: {out: {$bottom: {output: "$a", sortBy: {a: 1}}}}
        }
    }],
    [{
        $setWindowFields:
            {partitionBy: "$a", sortBy: {a: 1}, output: {out: {$minN: {input: "$a", n: 2}}}}
    }],
    [{
        $setWindowFields:
            {partitionBy: "$a", sortBy: {a: 1}, output: {out: {$maxN: {input: "$a", n: 2}}}}
    }],
    [{
        $setWindowFields:
            {partitionBy: "$a", sortBy: {a: 1}, output: {out: {$firstN: {input: "$a", n: 2}}}}
    }],
    [{
        $setWindowFields:
            {partitionBy: "$a", sortBy: {a: 1}, output: {out: {$lastN: {input: "$a", n: 2}}}}
    }],
    [{$setWindowFields: {partitionBy: "$a", sortBy: {a: 1}, output: {out: {$locf: "$a"}}}}],
    [{$densify: {field: "val", partitionByFields: ["a"], range: {step: 1, bounds: "partition"}}}],
];

for (const pipeline of stablePipelines) {
    // Assert running a pipeline with stages in API Version 1 succeeds.
    APIVersionHelpers.assertAggregateSucceedsWithAPIStrict(pipeline, collName);

    // Assert creating a view on a pipeline with stages in API Version 1 succeeds.
    APIVersionHelpers.assertViewSucceedsWithAPIStrict(pipeline, viewName, collName);

    // Assert error is not thrown when running without apiStrict=true.
    assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: pipeline,
        apiVersion: "1",
        cursor: {},
    }));
}
})();
