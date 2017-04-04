/**
 * Tests the behavior of operations when interacting with a view's default collation.
 */
(function() {
    "use strict";

    function runTest(conn) {
        let viewsDB = conn.getDB("views_collation");
        assert.commandWorked(viewsDB.dropDatabase());
        assert.commandWorked(viewsDB.runCommand({create: "simpleCollection"}));
        assert.commandWorked(
            viewsDB.runCommand({create: "ukCollection", collation: {locale: "uk"}}));
        assert.commandWorked(
            viewsDB.runCommand({create: "filCollection", collation: {locale: "fil"}}));

        // Creating a view without specifying a collation defaults to the simple collation.
        assert.commandWorked(viewsDB.runCommand({create: "simpleView", viewOn: "ukCollection"}));
        let listCollectionsOutput =
            viewsDB.runCommand({listCollections: 1, filter: {type: "view"}});
        assert.commandWorked(listCollectionsOutput);
        assert(!listCollectionsOutput.cursor.firstBatch[0].options.hasOwnProperty("collation"));

        // Operations that do not specify a collation succeed.
        assert.commandWorked(viewsDB.runCommand({aggregate: "simpleView", pipeline: []}));
        assert.commandWorked(viewsDB.runCommand({find: "simpleView"}));
        assert.commandWorked(viewsDB.runCommand({count: "simpleView"}));
        assert.commandWorked(viewsDB.runCommand({distinct: "simpleView", key: "x"}));

        // Operations that explicitly ask for the "simple" locale succeed against a view with the
        // simple collation.
        assert.commandWorked(viewsDB.runCommand(
            {aggregate: "simpleView", pipeline: [], collation: {locale: "simple"}}));
        assert.commandWorked(
            viewsDB.runCommand({find: "simpleView", collation: {locale: "simple"}}));
        assert.commandWorked(
            viewsDB.runCommand({count: "simpleView", collation: {locale: "simple"}}));
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

        // Operations with a matching collation succeed.
        assert.commandWorked(
            viewsDB.runCommand({aggregate: "filView", pipeline: [], collation: {locale: "fil"}}));
        assert.commandWorked(viewsDB.runCommand({find: "filView", collation: {locale: "fil"}}));
        assert.commandWorked(viewsDB.runCommand({count: "filView", collation: {locale: "fil"}}));
        assert.commandWorked(
            viewsDB.runCommand({distinct: "filView", key: "x", collation: {locale: "fil"}}));

        // Attempting to override the non-simple default collation of a view fails.
        assert.commandFailedWithCode(
            viewsDB.runCommand({aggregate: "filView", pipeline: [], collation: {locale: "en"}}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(
            viewsDB.runCommand({aggregate: "filView", pipeline: [], collation: {locale: "simple"}}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(
            viewsDB.runCommand({find: "filView", collation: {locale: "fr"}}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(
            viewsDB.runCommand({find: "filView", collation: {locale: "simple"}}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(
            viewsDB.runCommand({count: "filView", collation: {locale: "zh"}}),
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
        assert.commandWorked(viewsDB.runCommand({
            aggregate: "ukCollection",
            pipeline: [lookupSimpleView],
            collation: {locale: "simple"}
        }));
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
        assert.commandWorked(viewsDB.runCommand({
            aggregate: "ukCollection",
            pipeline: [graphLookupFilView],
            collation: {locale: "fil"}
        }));

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
        assert.commandFailedWithCode(viewsDB.runCommand({collMod: "esView", viewOn: "filView"}),
                                     ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(
            viewsDB.runCommand(
                {collMod: "esView", viewOn: "simpleCollection", pipeline: [lookupSimpleView]}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(
            viewsDB.runCommand(
                {collMod: "esView", viewOn: "simpleCollection", pipeline: [graphLookupFilView]}),
            ErrorCodes.OptionNotSupportedOnView);

        // Views cannot be dropped and recreated with a different collation if other views depend on
        // that view.
        assert.commandWorked(viewsDB.runCommand(
            {create: "filView2", viewOn: "filView", collation: {locale: "fil"}}));
        assert.commandWorked(viewsDB.runCommand({drop: "filView"}));
        assert.commandFailedWithCode(
            viewsDB.runCommand({create: "filView", viewOn: "simpleCollection"}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(
            viewsDB.runCommand(
                {create: "filView", viewOn: "simpleCollection", collation: {locale: "en"}}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandWorked(
            viewsDB.createView("filView", "ukCollection", [], {collation: {locale: "fil"}}));

        // Views cannot be dropped and recreated with a different collation if other views depend on
        // that view via $lookup or $graphLookup.
        assert.commandWorked(viewsDB.runCommand(
            {collMod: "filView2", viewOn: "simpleCollection", pipeline: [lookupFilView]}));
        assert.commandWorked(viewsDB.runCommand({drop: "filView"}));
        assert.commandFailedWithCode(
            viewsDB.runCommand({create: "filView", viewOn: "simpleCollection"}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(
            viewsDB.runCommand(
                {create: "filView", viewOn: "simpleCollection", collation: {locale: "en"}}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandWorked(viewsDB.runCommand(
            {create: "filView", viewOn: "ukCollection", pipeline: [], collation: {locale: "fil"}}));

        assert.commandWorked(viewsDB.runCommand(
            {collMod: "filView2", viewOn: "simpleCollection", pipeline: [graphLookupFilView]}));
        assert.commandWorked(viewsDB.runCommand({drop: "filView"}));
        assert.commandFailedWithCode(
            viewsDB.runCommand({create: "filView", viewOn: "simpleCollection"}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(
            viewsDB.runCommand(
                {create: "filView", viewOn: "simpleCollection", collation: {locale: "en"}}),
            ErrorCodes.OptionNotSupportedOnView);

        // If two views "A" and "C" have different collations and depend on the namespace "B", then
        // "B"
        // cannot be created as a view.
        assert.commandWorked(
            viewsDB.runCommand({create: "A", viewOn: "B", collation: {locale: "hsb"}}));
        assert.commandWorked(
            viewsDB.runCommand({create: "B", viewOn: "other", collation: {locale: "hsb"}}));
        assert.commandFailedWithCode(
            viewsDB.runCommand({create: "C", viewOn: "B", collation: {locale: "wae"}}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandWorked(viewsDB.runCommand({drop: "B"}));
        assert.commandWorked(
            viewsDB.runCommand({create: "C", viewOn: "B", collation: {locale: "wae"}}));
        assert.commandFailedWithCode(
            viewsDB.runCommand({create: "B", viewOn: "other", collation: {locale: "hsb"}}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(
            viewsDB.runCommand({create: "B", viewOn: "other", collation: {locale: "wae"}}),
            ErrorCodes.OptionNotSupportedOnView);
        assert.commandFailedWithCode(viewsDB.runCommand({create: "B", viewOn: "other"}),
                                     ErrorCodes.OptionNotSupportedOnView);
    }

    // Run the test on a standalone.
    let mongod = MongoRunner.runMongod({});
    runTest(mongod);
    MongoRunner.stopMongod(mongod);

    // Run the test on a sharded cluster.
    let cluster = new ShardingTest({shards: 1, mongos: 1});
    runTest(cluster);
    cluster.stop();
}());
