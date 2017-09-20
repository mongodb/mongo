/**
 * Tests the behavior of operations when interacting with a view's default collation.
 */
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    let viewsDB = db.getSiblingDB("views_collation");
    assert.commandWorked(viewsDB.dropDatabase());
    assert.commandWorked(viewsDB.runCommand({create: "simpleCollection"}));
    assert.commandWorked(viewsDB.runCommand({create: "ukCollection", collation: {locale: "uk"}}));
    assert.commandWorked(viewsDB.runCommand({create: "filCollection", collation: {locale: "fil"}}));

    // Creating a view without specifying a collation defaults to the simple collation.
    assert.commandWorked(viewsDB.runCommand({create: "simpleView", viewOn: "ukCollection"}));
    let listCollectionsOutput = viewsDB.runCommand({listCollections: 1, filter: {type: "view"}});
    assert.commandWorked(listCollectionsOutput);
    assert(!listCollectionsOutput.cursor.firstBatch[0].options.hasOwnProperty("collation"));

    // Operations that do not specify a collation succeed.
    assert.commandWorked(viewsDB.runCommand({aggregate: "simpleView", pipeline: []}));
    assert.commandWorked(viewsDB.runCommand({find: "simpleView"}));
    assert.commandWorked(viewsDB.runCommand({count: "simpleView"}));
    assert.commandWorked(viewsDB.runCommand({distinct: "simpleView", key: "x"}));

    // Operations that explicitly ask for the "simple" locale succeed against a view with the
    // simple collation.
    assert.commandWorked(
        viewsDB.runCommand({aggregate: "simpleView", pipeline: [], collation: {locale: "simple"}}));
    assert.commandWorked(viewsDB.runCommand({find: "simpleView", collation: {locale: "simple"}}));
    assert.commandWorked(viewsDB.runCommand({count: "simpleView", collation: {locale: "simple"}}));
    assert.commandWorked(
        viewsDB.runCommand({distinct: "simpleView", key: "x", collation: {locale: "simple"}}));

    // Attempting to override a view's simple collation fails.
    assert.commandFailedWithCode(
        viewsDB.runCommand({aggregate: "simpleView", pipeline: [], collation: {locale: "en"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand({find: "simpleView", collation: {locale: "fr"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand({count: "simpleView", collation: {locale: "fil"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand({distinct: "simpleView", key: "x", collation: {locale: "es"}}),
        ErrorCodes.OptionNotSupportedOnView);

    // Create a view with an explicit, non-simple collation.
    assert.commandWorked(
        viewsDB.createView("filView", "ukCollection", [], {collation: {locale: "fil"}}));
    listCollectionsOutput = viewsDB.runCommand({listCollections: 1, filter: {name: "filView"}});
    assert.commandWorked(listCollectionsOutput);
    assert.eq(listCollectionsOutput.cursor.firstBatch[0].options.collation.locale, "fil");

    // Operations that do not specify a collation succeed.
    assert.commandWorked(viewsDB.runCommand({aggregate: "filView", pipeline: []}));
    assert.commandWorked(viewsDB.runCommand({find: "filView"}));
    assert.commandWorked(viewsDB.runCommand({count: "filView"}));
    assert.commandWorked(viewsDB.runCommand({distinct: "filView", key: "x"}));

    // Explain of operations that do not specify a collation succeed.
    assert.commandWorked(viewsDB.runCommand({aggregate: "filView", pipeline: [], explain: true}));
    assert.commandWorked(
        viewsDB.runCommand({explain: {find: "filView"}, verbosity: "allPlansExecution"}));
    assert.commandWorked(
        viewsDB.runCommand({explain: {count: "filView"}, verbosity: "allPlansExecution"}));
    assert.commandWorked(viewsDB.runCommand(
        {explain: {distinct: "filView", key: "x"}, verbosity: "allPlansExecution"}));

    // Operations with a matching collation succeed.
    assert.commandWorked(
        viewsDB.runCommand({aggregate: "filView", pipeline: [], collation: {locale: "fil"}}));
    assert.commandWorked(viewsDB.runCommand({find: "filView", collation: {locale: "fil"}}));
    assert.commandWorked(viewsDB.runCommand({count: "filView", collation: {locale: "fil"}}));
    assert.commandWorked(
        viewsDB.runCommand({distinct: "filView", key: "x", collation: {locale: "fil"}}));

    // Explain of operations with a matching collation succeed.
    assert.commandWorked(viewsDB.runCommand(
        {aggregate: "filView", pipeline: [], explain: true, collation: {locale: "fil"}}));
    assert.commandWorked(viewsDB.runCommand(
        {explain: {find: "filView", collation: {locale: "fil"}}, verbosity: "allPlansExecution"}));
    assert.commandWorked(viewsDB.runCommand(
        {explain: {count: "filView", collation: {locale: "fil"}}, verbosity: "allPlansExecution"}));
    assert.commandWorked(viewsDB.runCommand({
        explain: {distinct: "filView", key: "x", collation: {locale: "fil"}},
        verbosity: "allPlansExecution"
    }));

    // Attempting to override the non-simple default collation of a view fails.
    assert.commandFailedWithCode(
        viewsDB.runCommand({aggregate: "filView", pipeline: [], collation: {locale: "en"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand({aggregate: "filView", pipeline: [], collation: {locale: "simple"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({find: "filView", collation: {locale: "fr"}}),
                                 ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand({find: "filView", collation: {locale: "simple"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({count: "filView", collation: {locale: "zh"}}),
                                 ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand({count: "filView", collation: {locale: "simple"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand({distinct: "filView", key: "x", collation: {locale: "es"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand({distinct: "filView", key: "x", collation: {locale: "simple"}}),
        ErrorCodes.OptionNotSupportedOnView);

    // Attempting to override the default collation of a view with explain fails.
    assert.commandFailedWithCode(
        viewsDB.runCommand(
            {aggregate: "filView", pipeline: [], explain: true, collation: {locale: "en"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand(
            {aggregate: "filView", pipeline: [], explain: true, collation: {locale: "simple"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        explain: {find: "filView", collation: {locale: "fr"}},
        verbosity: "allPlansExecution"
    }),
                                 ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        explain: {find: "filView", collation: {locale: "simple"}},
        verbosity: "allPlansExecution"
    }),
                                 ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        explain: {count: "filView", collation: {locale: "zh"}},
        verbosity: "allPlansExecution"
    }),
                                 ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        explain: {count: "filView", collation: {locale: "simple"}},
        verbosity: "allPlansExecution"
    }),
                                 ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        explain: {distinct: "filView", key: "x", collation: {locale: "es"}},
        verbosity: "allPlansExecution"
    }),
                                 ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        explain: {distinct: "filView", key: "x", collation: {locale: "simple"}},
        verbosity: "allPlansExecution"
    }),
                                 ErrorCodes.OptionNotSupportedOnView);

    const lookupSimpleView = {
        $lookup: {from: "simpleView", localField: "x", foreignField: "x", as: "result"}
    };
    const graphLookupSimpleView = {
        $graphLookup: {
            from: "simpleView",
            startWith: "$_id",
            connectFromField: "_id",
            connectToField: "matchedId",
            as: "matched"
        }
    };

    // You can lookup into a view with the simple collation if the collection also has the same
    // default collation.
    assert.commandWorked(
        viewsDB.runCommand({aggregate: "simpleCollection", pipeline: [lookupSimpleView]}));
    assert.commandWorked(
        viewsDB.runCommand({aggregate: "simpleCollection", pipeline: [graphLookupSimpleView]}));

    // You can lookup into a view with the simple collation if the operation has a matching
    // collation.
    assert.commandWorked(viewsDB.runCommand(
        {aggregate: "ukCollection", pipeline: [lookupSimpleView], collation: {locale: "simple"}}));
    assert.commandWorked(viewsDB.runCommand({
        aggregate: "ukCollection",
        pipeline: [graphLookupSimpleView],
        collation: {locale: "simple"}
    }));

    // You can't lookup into a view with the simple collation if the operation has a conflicting
    // collation.
    assert.commandFailedWithCode(viewsDB.runCommand({
        aggregate: "simpleCollection",
        pipeline: [lookupSimpleView],
        collation: {locale: "en"}
    }),
                                 ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        aggregate: "simpleCollection",
        pipeline: [graphLookupSimpleView],
        collation: {locale: "zh"}
    }),
                                 ErrorCodes.OptionNotSupportedOnView);

    const lookupFilView = {
        $lookup: {from: "filView", localField: "x", foreignField: "x", as: "result"}
    };
    const graphLookupFilView = {
        $graphLookup: {
            from: "filView",
            startWith: "$_id",
            connectFromField: "_id",
            connectToField: "matchedId",
            as: "matched"
        }
    };

    // You can lookup into a view with no operation collation specified if the collection's
    // collation matches the collation of the view.
    assert.commandWorked(
        viewsDB.runCommand({aggregate: "filCollection", pipeline: [lookupFilView]}));
    assert.commandWorked(
        viewsDB.runCommand({aggregate: "filCollection", pipeline: [graphLookupFilView]}));

    // You can lookup into a view with a non-simple collation if the operation's collation
    // matches.
    assert.commandWorked(viewsDB.runCommand(
        {aggregate: "ukCollection", pipeline: [lookupFilView], collation: {locale: "fil"}}));
    assert.commandWorked(viewsDB.runCommand(
        {aggregate: "ukCollection", pipeline: [graphLookupFilView], collation: {locale: "fil"}}));

    // You can't lookup into a view when aggregating a collection whose default collation does
    // not match the view's default collation.
    assert.commandFailedWithCode(
        viewsDB.runCommand({aggregate: "simpleCollection", pipeline: [lookupFilView]}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand({aggregate: "simpleCollection", pipeline: [graphLookupFilView]}),
        ErrorCodes.OptionNotSupportedOnView);

    // You can't lookup into a view when aggregating a collection and the operation's collation
    // does not match the view's default collation.
    assert.commandFailedWithCode(
        viewsDB.runCommand(
            {aggregate: "filCollection", pipeline: [lookupFilView], collation: {locale: "zh"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        aggregate: "filCollection",
        pipeline: [graphLookupFilView],
        collation: {locale: "zh"}
    }),
                                 ErrorCodes.OptionNotSupportedOnView);

    // You may perform an aggregation involving multiple views if they all have the same default
    // collation.
    assert.commandWorked(viewsDB.runCommand(
        {create: "simpleView2", viewOn: "simpleCollection", collation: {locale: "simple"}}));
    assert.commandWorked(
        viewsDB.runCommand({aggregate: "simpleView2", pipeline: [lookupSimpleView]}));
    assert.commandWorked(
        viewsDB.runCommand({aggregate: "simpleView2", pipeline: [graphLookupSimpleView]}));

    // You may perform an aggregation involving multiple views and collections if all the views
    // have the same default collation.
    const graphLookupUkCollection = {
        $graphLookup: {
            from: "ukCollection",
            startWith: "$_id",
            connectFromField: "_id",
            connectToField: "matchedId",
            as: "matched"
        }
    };
    assert.commandWorked(viewsDB.runCommand(
        {aggregate: "simpleView2", pipeline: [lookupSimpleView, graphLookupUkCollection]}));

    // You cannot perform an aggregation involving multiple views if the views don't all have
    // the same default collation.
    assert.commandFailedWithCode(
        viewsDB.runCommand({aggregate: "filView", pipeline: [lookupSimpleView]}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand({aggregate: "simpleView", pipeline: [lookupFilView]}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand(
            {aggregate: "simpleCollection", pipeline: [lookupFilView, graphLookupSimpleView]}),
        ErrorCodes.OptionNotSupportedOnView);

    // You cannot create a view that depends on another view with a different default collation.
    assert.commandFailedWithCode(
        viewsDB.runCommand({create: "zhView", viewOn: "filView", collation: {locale: "zh"}}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        create: "zhView",
        viewOn: "simpleCollection",
        pipeline: [lookupFilView],
        collation: {locale: "zh"}
    }),
                                 ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        create: "zhView",
        viewOn: "simpleCollection",
        pipeline: [graphLookupSimpleView],
        collation: {locale: "zh"}
    }),
                                 ErrorCodes.OptionNotSupportedOnView);

    // You cannot modify a view to depend on another view with a different default collation.
    assert.commandWorked(viewsDB.runCommand(
        {create: "esView", viewOn: "simpleCollection", collation: {locale: "es"}}));
    assert.commandFailedWithCode(
        viewsDB.runCommand({collMod: "esView", viewOn: "filView", pipeline: []}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand(
            {collMod: "esView", viewOn: "simpleCollection", pipeline: [lookupSimpleView]}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        viewsDB.runCommand(
            {collMod: "esView", viewOn: "simpleCollection", pipeline: [graphLookupFilView]}),
        ErrorCodes.OptionNotSupportedOnView);

    // Make sure that when an operation does not specify the collation, it correctly uses the
    // default collation associated with the view. For this, we set up a new backing collection with
    // a case-insensitive view.
    assert.commandWorked(viewsDB.runCommand({create: "case_sensitive_coll"}));
    assert.commandWorked(viewsDB.runCommand({
        create: "case_insensitive_view",
        viewOn: "case_sensitive_coll",
        collation: {locale: "en", strength: 1}
    }));

    assert.writeOK(viewsDB.case_sensitive_coll.insert({f: "case"}));
    assert.writeOK(viewsDB.case_sensitive_coll.insert({f: "Case"}));
    assert.writeOK(viewsDB.case_sensitive_coll.insert({f: "CASE"}));

    let explain, cursorStage;

    // Test that aggregate against a view with a default collation correctly uses the collation.
    assert.eq(1, viewsDB.case_sensitive_coll.aggregate([{$match: {f: "case"}}]).itcount());
    assert.eq(3, viewsDB.case_insensitive_view.aggregate([{$match: {f: "case"}}]).itcount());
    explain = viewsDB.case_insensitive_view.explain().aggregate([{$match: {f: "case"}}]);
    cursorStage = getAggPlanStage(explain, "$cursor");
    assert.neq(null, cursorStage, tojson(explain));
    assert.eq(1, cursorStage.$cursor.queryPlanner.collation.strength, tojson(cursorStage));

    // Test that count against a view with a default collation correctly uses the collation.
    assert.eq(1, viewsDB.case_sensitive_coll.count({f: "case"}));
    assert.eq(3, viewsDB.case_insensitive_view.count({f: "case"}));
    explain = viewsDB.case_insensitive_view.explain().count({f: "case"});
    cursorStage = getAggPlanStage(explain, "$cursor");
    assert.neq(null, cursorStage, tojson(explain));
    assert.eq(1, cursorStage.$cursor.queryPlanner.collation.strength, tojson(cursorStage));

    // Test that distinct against a view with a default collation correctly uses the collation.
    assert.eq(3, viewsDB.case_sensitive_coll.distinct("f").length);
    assert.eq(1, viewsDB.case_insensitive_view.distinct("f").length);
    explain = viewsDB.case_insensitive_view.explain().distinct("f");
    cursorStage = getAggPlanStage(explain, "$cursor");
    assert.neq(null, cursorStage, tojson(explain));
    assert.eq(1, cursorStage.$cursor.queryPlanner.collation.strength, tojson(cursorStage));

    // Test that find against a view with a default collation correctly uses the collation.
    let findRes = viewsDB.runCommand({find: "case_sensitive_coll", filter: {f: "case"}});
    assert.commandWorked(findRes);
    assert.eq(1, findRes.cursor.firstBatch.length);
    findRes = viewsDB.runCommand({find: "case_insensitive_view", filter: {f: "case"}});
    assert.commandWorked(findRes);
    assert.eq(3, findRes.cursor.firstBatch.length);
    explain = viewsDB.runCommand({explain: {find: "case_insensitive_view", filter: {f: "case"}}});
    cursorStage = getAggPlanStage(explain, "$cursor");
    assert.neq(null, cursorStage, tojson(explain));
    assert.eq(1, cursorStage.$cursor.queryPlanner.collation.strength, tojson(cursorStage));
}());
