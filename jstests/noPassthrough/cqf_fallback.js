/**
 * Verify that expressions and operators are correctly routed to CQF where eligible. This decision
 * is based on several factors including the query text, collection metadata, etc..
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/optimizer_utils.js");

let conn = MongoRunner.runMongod({setParameter: {featureFlagCommonQueryFramework: true}});
assert.neq(null, conn, "mongod was unable to start up");

let db = conn.getDB("test");
let coll = db[jsTestName()];
coll.drop();

// This test relies on the bonsai optimizer being enabled.
if (assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl == "forceClassicEngine") {
    jsTestLog("Skipping test due to forceClassicEngine");
    MongoRunner.stopMongod(conn);
    return;
}

function assertUsesFallback(cmd, testOnly) {
    // An unsupported stage should not use the new optimizer.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
    const defaultExplain = assert.commandWorked(db.runCommand({explain: cmd}));
    assert(!usedBonsaiOptimizer(defaultExplain), tojson(defaultExplain));

    // Non-explain should also work and use the fallback mechanism, but we cannnot verify exactly
    // this without looking at the logs.
    assert.commandWorked(db.runCommand(cmd));

    // Force the bonsai optimizer and expect the query to either fail if unsupported, or pass if
    // marked as "test only".
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
    if (testOnly) {
        const explain = assert.commandWorked(db.runCommand({explain: cmd}));
        assert(usedBonsaiOptimizer(explain), tojson(explain));
    } else {
        assert.commandFailedWithCode(db.runCommand(cmd), ErrorCodes.InternalErrorNotSupported);
    }

    // Forcing the classic engine should not use Bonsai.
    {
        assert.commandWorked(db.adminCommand(
            {setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
        const explain = assert.commandWorked(db.runCommand({explain: cmd}));
        assert(!usedBonsaiOptimizer(explain), tojson(explain));
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
    }
}

// Unsupported aggregation stage.
assertUsesFallback({aggregate: coll.getName(), pipeline: [{$sample: {size: 1}}], cursor: {}},
                   false);

// Test-only aggregation stage.
assertUsesFallback(
    {aggregate: coll.getName(), pipeline: [{$group: {_id: null, a: {$sum: "$b"}}}], cursor: {}},
    true);

// Unsupported match expression.
assertUsesFallback({find: coll.getName(), filter: {a: {$mod: [4, 0]}}}, false);
assertUsesFallback(
    {aggregate: coll.getName(), pipeline: [{$match: {a: {$mod: [4, 0]}}}], cursor: {}}, false);
assertUsesFallback({find: coll.getName(), filter: {a: {$in: [/^b/, 1]}}}, false);

// Test-only match expression.
assertUsesFallback({find: coll.getName(), filter: {$alwaysFalse: 1}}, true);
assertUsesFallback({aggregate: coll.getName(), pipeline: [{$match: {$alwaysFalse: 1}}], cursor: {}},
                   true);

// Unsupported projection expression.
assertUsesFallback(
    {find: coll.getName(), filter: {}, projection: {a: {$concatArrays: [["$b"], ["suppported"]]}}},
    false);
assertUsesFallback({
    aggregate: coll.getName(),
    pipeline: [{$project: {a: {$concatArrays: [["$b"], ["suppported"]]}}}],
    cursor: {}
},
                   false);

// Test-only projection spec.
assertUsesFallback(
    {find: coll.getName(), filter: {}, projection: {a: {$concat: ["test", "-only"]}}}, true);
assertUsesFallback({
    aggregate: coll.getName(),
    pipeline: [{$project: {a: {$concat: ["test", "-only"]}}}],
    cursor: {}
},
                   true);

// Numeric path components are not supported, either in a match expression or projection.
assertUsesFallback({find: coll.getName(), filter: {'a.0': 5}});
assertUsesFallback({find: coll.getName(), filter: {'a.0.b': 5}});
assertUsesFallback({find: coll.getName(), filter: {}, projection: {'a.0': 1}});
assertUsesFallback({find: coll.getName(), filter: {}, projection: {'a.5.c': 0}});

// Test for unsupported expressions within a branching expression such as $or.
assertUsesFallback({find: coll.getName(), filter: {$or: [{'a.0': 5}, {a: 1}]}});
assertUsesFallback({find: coll.getName(), filter: {$or: [{a: 5}, {a: {$mod: [4, 0]}}]}});

// Unsupported command options.
assertUsesFallback({find: coll.getName(), filter: {}, collation: {locale: "fr_CA"}}, true);
assertUsesFallback({
    aggregate: coll.getName(),
    pipeline: [{$match: {$alwaysFalse: 1}}],
    collation: {locale: "fr_CA"},
    cursor: {}
},
                   true);

// Unsupported index type.
assert.commandWorked(coll.createIndex({a: 1}, {sparse: true}));
assertUsesFallback({find: coll.getName(), filter: {}});
assertUsesFallback({aggregate: coll.getName(), pipeline: [], cursor: {}});
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.createIndex({"$**": 1}));
assertUsesFallback({find: coll.getName(), filter: {}});
assertUsesFallback({aggregate: coll.getName(), pipeline: [], cursor: {}});

// Test-only index type.
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {a: {$gt: 0}}}));
assertUsesFallback({find: coll.getName(), filter: {}}, true);
assertUsesFallback({aggregate: coll.getName(), pipeline: [], cursor: {}}, true);

// Unsupported collection types. Note that a query against the user-facing timeseries collection
// will fail due to the unsupported $unpackBucket stage.
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "time"}}));
assertUsesFallback({find: coll.getName(), filter: {}}, false);
assertUsesFallback({aggregate: coll.getName(), pipeline: [], cursor: {}}, false);

const bucketColl = db.getCollection('system.buckets.' + coll.getName());
assertUsesFallback({find: bucketColl.getName(), filter: {}}, false);
assertUsesFallback({aggregate: bucketColl.getName(), pipeline: [], cursor: {}}, false);

// Collection-default collation is not supported if non-simple.
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
assertUsesFallback({find: coll.getName(), filter: {}}, false);
assertUsesFallback({aggregate: coll.getName(), pipeline: [], cursor: {}}, false);

// Queries over views are supported as long as the resolved pipeline is valid in CQF.
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(
    db.runCommand({create: "view", viewOn: coll.getName(), pipeline: [{$match: {a: 1}}]}));

// Unsupported expression on top of the view.
assertUsesFallback({find: "view", filter: {a: {$mod: [4, 0]}}}, false);

// Supported expression on top of the view.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
assert.commandWorked(db.runCommand({find: "view", filter: {b: 4}}));

// Test-only expression on top of a view.
assertUsesFallback({find: "view", filter: {$alwaysFalse: 1}}, true);

// Create a view with an unsupported expression.
assert.commandWorked(db.runCommand(
    {create: "invalidView", viewOn: coll.getName(), pipeline: [{$match: {a: {$mod: [4, 0]}}}]}));

// Any expression, supported or not, should not use CQF over the invalid view.
assertUsesFallback({find: "invalidView", filter: {b: 4}}, false);

// Test only expression should also fail.
assertUsesFallback({find: "invalidView", filter: {$alwaysFalse: 1}}, true);

MongoRunner.stopMongod(conn);

// Restart the mongod and verify that we never use the bonsai optimizer if the feature flag is not
// set.
conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

db = conn.getDB("test");
coll = db[jsTestName()];
coll.drop();

const supportedExpression = {
    a: {$eq: 4}
};

let explain = coll.explain().find(supportedExpression).finish();
assert(!usedBonsaiOptimizer(explain), tojson(explain));

explain = coll.explain().aggregate([{$match: supportedExpression}]);
assert(!usedBonsaiOptimizer(explain), tojson(explain));

// Setting the force Bonsai flag has no effect.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
explain = coll.explain().find(supportedExpression).finish();
assert(!usedBonsaiOptimizer(explain), tojson(explain));

explain = coll.explain().aggregate([{$match: supportedExpression}]);
assert(!usedBonsaiOptimizer(explain), tojson(explain));

MongoRunner.stopMongod(conn);
}());