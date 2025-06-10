// @tags: [
//   # Running getCollection on views in sharded suites tries to shard views, which fails.
//   assumes_unsharded_collection,
//   requires_non_retryable_commands,
//   references_foreign_collection,
// ]

let viewsDb = db.getSiblingDB("views_validation");
const kMaxViewDepth = 20;

function makeView(viewName, viewOn, pipeline, expectedErrorCode) {
    let options = {create: viewName, viewOn: viewOn};
    if (pipeline) {
        options["pipeline"] = pipeline;
    }
    let res = viewsDb.runCommand(options);
    if (expectedErrorCode !== undefined) {
        assert.commandFailedWithCode(
            res, expectedErrorCode, "Invalid view created " + tojson(options));
    } else {
        assert.commandWorked(res, "Could not create view " + tojson(options));
    }
}

function makeLookup(from) {
    return {
        $lookup: {from: from, as: "as", localField: "localField", foreignField: "foreignField"}
    };
}

function makeGraphLookup(from) {
    return {
            $graphLookup: {
                from: from,
                as: "as",
                startWith: "startWith",
                connectFromField: "connectFromField",
                connectToField: "connectToField"
            }
        };
}

function makeFacet(from) {
    return {$facet: {"Facet Key": [makeLookup(from)]}};
}

function makeUnion(from) {
    return {$unionWith: from};
}

function clear() {
    assert.commandWorked(viewsDb.dropDatabase());
}

clear();

// Check that simple cycles are disallowed.
makeView("a", "a", [], ErrorCodes.GraphContainsCycle);
clear();

makeView("a", "b");
makeView("b", "a", [], ErrorCodes.GraphContainsCycle);
clear();

makeView("a", "b");
makeView("b", "c");
makeView("c", "a", [], ErrorCodes.GraphContainsCycle);
clear();

// Test that $lookup checks for cycles.
makeView("a", "b", [makeLookup("a")], ErrorCodes.GraphContainsCycle);
clear();

makeView("a", "b");
makeView("b", "c", [makeLookup("a")], ErrorCodes.GraphContainsCycle);
clear();

// Test that $graphLookup checks for cycles.
makeView("a", "b", [makeGraphLookup("a")], ErrorCodes.GraphContainsCycle);
makeView("a", "b", [makeGraphLookup("b")]);
makeView("b", "c", [makeGraphLookup("a")], ErrorCodes.GraphContainsCycle);
clear();

// Test that $facet checks for cycles.
makeView("a", "b", [makeFacet("a")], ErrorCodes.GraphContainsCycle);
makeView("a", "b", [makeFacet("b")]);
makeView("b", "c", [makeFacet("a")], ErrorCodes.GraphContainsCycle);
clear();

// Test that $unionWith checks for cycles.
makeView("a", "b", [makeUnion("a")], ErrorCodes.GraphContainsCycle);
makeView("a", "b", [makeUnion("b")]);
makeView("b", "c", [makeUnion("a")], ErrorCodes.GraphContainsCycle);
clear();

// Test that $unionWith checks for cycles within a nested $unionWith.
makeView("a",
         "b",
         [{$unionWith: {coll: "c", pipeline: [{$unionWith: "a"}]}}],
         ErrorCodes.GraphContainsCycle);

/*
 * Check that view validation does not naively recurse on already visited views.
 *
 * Make a tree of depth 20 as with one view per level follows:
 *                     1
 *       -----------------------------
 *      2         2         2         2
 *    -----     -----     -----     -----
 *   3 3 3 3   3 3 3 3   3 3 3 3   3 3 3 3
 *     ...       ...       ...       ...
 *
 * So view i depends on the view (i+1) four times. Since it should only need to recurse
 * down one branch completely for each creation, since this should only need to check a maximum
 * of 20 views instead of 4^20 views.
 */

for (let i = 1; i <= kMaxViewDepth; i++) {
    let childView = "v" + (i + 1);
    makeView("v" + i, childView, [
        makeLookup(childView),
        makeGraphLookup(childView),
        makeFacet(childView),
        makeUnion(childView),
    ]);
}

// Check that any higher depth leads to failure
makeView("v21", "v22", [], ErrorCodes.ViewDepthLimitExceeded);
makeView("v0", "v1", [], ErrorCodes.ViewDepthLimitExceeded);
makeView("v0", "ok", [makeLookup("v1")], ErrorCodes.ViewDepthLimitExceeded);
makeView("v0", "ok", [makeGraphLookup("v1")], ErrorCodes.ViewDepthLimitExceeded);
makeView("v0", "ok", [makeFacet("v1")], ErrorCodes.ViewDepthLimitExceeded);
makeView("v0", "ok", [makeUnion("v1")], ErrorCodes.ViewDepthLimitExceeded);

// Test that querying a view that descends more than 20 views will fail.

// Run the initial aggregate 'kMaxRetries' times. If this is a sharded cluster and the targeted node
// doesn't have the necessary routing information to detect that the view is invalid, this will
// throw a StaleConfig error instead. In doing so it will obtain the routing information for 10 of
// our views (one on each attempt), which will allow the subsequent aggregates to discover that the
// view chain is 20 deep and fail as expected. We run it at most 'kMaxRetries' times in the event
// that we have multiple nodes in our cluster that can answer this query. We expect that,
// eventually, we'll detect a 'ViewDepthLimitExceeded' error.
// TODO SERVER-85941: This ticket aims to prevent needing extra aggregates to get some of the
// routing information.
function detectViewError(aggStage) {
    const kMaxRetries = 6;
    let detectedViewError = false;
    for (let i = 0; i < kMaxRetries; ++i) {
        const res = viewsDb.runCommand({aggregate: "v10", pipeline: [aggStage], cursor: {}});
        assert.commandFailedWithCode(res,
                                     [ErrorCodes.ViewDepthLimitExceeded, ErrorCodes.StaleConfig]);
        if (res.code === ErrorCodes.ViewDepthLimitExceeded) {
            detectedViewError = true;
            break;
        }
    }
    assert(detectedViewError,
           "Did not detect view error after " + kMaxRetries + " retries of the aggregation");
}

detectViewError(makeUnion("v1"));
detectViewError(makeLookup("v1"));

// But adding to the middle should be ok.
makeView("vMid", "v10");
clear();

// Check that collMod also checks for cycles.
makeView("a", "b");
makeView("b", "c");
assert.commandFailedWithCode(viewsDb.runCommand({collMod: "b", viewOn: "a", pipeline: []}),
                             ErrorCodes.GraphContainsCycle,
                             "collmod changed view to create a cycle");

// Check that collMod disallows the specification of invalid pipelines.
assert.commandFailedWithCode(
    viewsDb.runCommand({collMod: "b", viewOn: "c", pipeline: {}}),
    ErrorCodes.TypeMismatch,
    "BSON field 'collMod.pipeline' is the wrong type 'object', expected type 'array'");
assert.commandFailedWithCode(
    viewsDb.runCommand({collMod: "b", viewOn: "c", pipeline: {0: {$limit: 7}}}),
    ErrorCodes.TypeMismatch,
    "BSON field 'collMod.pipeline' is the wrong type 'object', expected type 'array'");
clear();

// Check that collMod disallows the 'expireAfterSeconds' option over a view.
makeView("a", "b");
assert.commandFailedWithCode(viewsDb.runCommand({collMod: "a", expireAfterSeconds: 1}),
                             ErrorCodes.InvalidOptions);
clear();

// For the assert below with multiple error codes, SERVER-93055 changes the parse error codes to an
// IDL code, as these errors are now caught during IDL parsing. However, due to these tests being
// run across multiple versions, we need to allow both types of errors.

// Check that invalid pipelines are disallowed. The following $lookup is missing the 'as' field.
// TODO SERVER-106081: Remove ErrorCodes.FailedToParse.
makeView("a",
         "b",
         [{"$lookup": {from: "a", localField: "b", foreignField: "c"}}],
         [ErrorCodes.IDLFailedToParse, ErrorCodes.FailedToParse]);

// Check that free variables in view pipeline are disallowed.
makeView("a", "b", [{"$project": {field: "$$undef"}}], 17276);
makeView("a", "b", [{"$addFields": {field: "$$undef"}}], 17276);

const invalidDb = db.getSiblingDB("$gt");
assert.commandFailedWithCode(
    invalidDb.createView('testView', 'testColl', []),
    [17320, ErrorCodes.InvalidNamespace, ErrorCodes.InvalidViewDefinition]);
// Delete the invalid view (by dropping the database) so that the validate hook succeeds.
assert.commandWorked(invalidDb.dropDatabase());
