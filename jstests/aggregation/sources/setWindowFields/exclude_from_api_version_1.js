/**
 * Test that $setWindowFields is excluded from API version 1.
 */
(function() {
"use strict";

const coll = db[jsTestName()];
coll.drop();
coll.insert({a: 1});
coll.insert({a: 2});

assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {_id: 1},
            output: {runningCount: {$sum: 1, window: {documents: ["unbounded", "current"]}}}
        }
    }],
    cursor: {},
    apiStrict: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIStrictError);

const viewName = coll.getName() + "_window_view";
// Make sure to manually run the drop command instead of using the .drop() helper because the
// .drop() helper may re-create the collection as sharded in certain passthrough suites.
assert.commandWorkedOrFailedWithCode(db.runCommand({drop: viewName}), ErrorCodes.NamespaceNotFound);
// Test that we cannot store it in a view.
assert.commandFailedWithCode(db.runCommand({
    create: viewName,
    viewOn: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {_id: 1},
            output: {runningCount: {$sum: 1, window: {documents: ["unbounded", "current"]}}}
        }
    }],
    apiStrict: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIStrictError);

// Test that if $setWindowFields is stored in a view, we cannot query the view with apiVersion "1"
// and apiStrict=true.
assert.commandWorked(db.runCommand({
    create: viewName,
    viewOn: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {_id: 1},
            output: {runningCount: {$sum: 1, window: {documents: ["unbounded", "current"]}}}
        }
    }]
}));
assert.commandFailedWithCode(
    db.runCommand({find: viewName, filter: {}, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: viewName, pipeline: [], cursor: {}, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);

// Test that $count is included from API Version 1 so long as it's used in $group.
assert.doesNotThrow(() => coll.aggregate([{$group: {_id: null, count: {$count: {}}}}],
                                         {apiVersion: "1", apiStrict: true}));
})();
