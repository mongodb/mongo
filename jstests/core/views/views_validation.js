(function() {
    "use strict";
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

        return viewsDb.getCollection(viewName);
    }

    function makeLookup(from) {
        return {
            $lookup:
                {from: from, as: "as", localField: "localField", foreignField: "foreignField"}
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

    function clear() {
        assert.commandWorked(viewsDb.dropDatabase());
    }

    clear();

    // Check that simple cycles are disallowed.
    makeView("a", "a", [], ErrorCodes.GraphContainsCycle);
    makeView("a", "b", [makeLookup("a")], ErrorCodes.GraphContainsCycle);
    clear();

    makeView("a", "b", ErrorCodes.OK);
    makeView("b", "a", [], ErrorCodes.GraphContainsCycle);
    makeView("b", "c", [makeLookup("a")], ErrorCodes.GraphContainsCycle);
    clear();

    makeView("a", "b");
    makeView("b", "c");
    makeView("c", "a", [], ErrorCodes.GraphContainsCycle);
    clear();

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
        makeView("v" + i,
                 childView,
                 [makeLookup(childView), makeGraphLookup(childView), makeFacet(childView)]);
    }

    // Check that any higher depth leads to failure
    makeView("v21", "v22", [], ErrorCodes.ViewDepthLimitExceeded);
    makeView("v0", "v1", [], ErrorCodes.ViewDepthLimitExceeded);
    makeView("v0", "ok", [makeLookup("v1")], ErrorCodes.ViewDepthLimitExceeded);

    // But adding to the middle should be ok.
    makeView("vMid", "v10");
    clear();

    // Check that $graphLookup and $facet also check for cycles.
    makeView("a", "b", [makeGraphLookup("a")], ErrorCodes.GraphContainsCycle);
    makeView("a", "b", [makeGraphLookup("b")]);
    makeView("b", "c", [makeGraphLookup("a")], ErrorCodes.GraphContainsCycle);
    clear();

    makeView("a", "b", [makeFacet("a")], ErrorCodes.GraphContainsCycle);
    makeView("a", "b", [makeFacet("b")]);
    makeView("b", "c", [makeFacet("a")], ErrorCodes.GraphContainsCycle);
    clear();

    // Check that collMod also checks for cycles.
    makeView("a", "b");
    makeView("b", "c");
    assert.commandFailedWithCode(viewsDb.runCommand({collMod: "b", viewOn: "a", pipeline: []}),
                                 ErrorCodes.GraphContainsCycle,
                                 "collmod changed view to create a cycle");

    // Check that collMod disallows the specification of invalid pipelines.
    assert.commandFailedWithCode(viewsDb.runCommand({collMod: "b", viewOn: "c", pipeline: {}}),
                                 ErrorCodes.InvalidOptions,
                                 "collMod modified view to have invalid pipeline");
    assert.commandFailedWithCode(
        viewsDb.runCommand({collMod: "b", viewOn: "c", pipeline: {0: {$limit: 7}}}),
        ErrorCodes.InvalidOptions,
        "collMod modified view to have invalid pipeline");
    clear();

    // Check that invalid pipelines are disallowed.
    makeView("a", "b", [{"$lookup": {from: "a"}}], 4572);  // 4572 is for missing $lookup fields
}());
