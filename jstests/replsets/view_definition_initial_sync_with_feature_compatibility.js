/**
 * Test that a new replica set member can successfully sync a collection with a view using 4.0
 * aggregation features, even when the replica set was downgraded to feature compatibility version
 * 3.6.
 *
 * TODO SERVER-33321: Remove FCV 3.6 validation during the 4.1 development cycle.
 *
 * We restart the secondary as part of this test with the expectation that it still has the same
 * data after the restart.
 * @tags: [requires_persistence]
 */

load("jstests/replsets/rslib.js");

(function() {
    "use strict";
    const testName = "view_definition_initial_sync_with_feature_compatibility";

    function testView(pipeline) {
        //
        // Create a single-node replica set.
        //
        let replTest = new ReplSetTest({name: testName, nodes: 1});

        replTest.startSet();
        replTest.initiate();

        let primary = replTest.getPrimary();
        let testDB = primary.getDB("test");

        //
        // Explicitly set the replica set to feature compatibility version 4.0.
        //
        assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

        //
        // Create a view using 4.0 query features.
        //
        assert.commandWorked(testDB.createView("view1", "coll", pipeline));

        //
        // Downgrade the replica set to feature compatibility version 3.6.
        //
        assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

        //
        // Add a new member to the replica set.
        //
        let secondaryDBPath = MongoRunner.dataPath + testName + "_secondary";
        resetDbpath(secondaryDBPath);
        let secondary = replTest.add({dbpath: secondaryDBPath});
        replTest.reInitiate(secondary);
        reconnect(primary);
        reconnect(secondary);

        //
        // Once the new member completes its initial sync, stop it, remove it from the replica
        // set, and start it back up as an individual instance.
        //
        replTest.waitForState(secondary, [ReplSetTest.State.PRIMARY, ReplSetTest.State.SECONDARY]);

        replTest.stopSet(undefined /* send default signal */,
                         true /* don't clear data directory */);

        secondary = MongoRunner.runMongod({dbpath: secondaryDBPath, noCleanData: true});
        assert.neq(null, secondary, "mongod was unable to start up");

        //
        // Verify that the view synced to the new member.
        //
        let secondaryDB = secondary.getDB("test");
        assert.eq(secondaryDB.system.views.findOne({_id: "test.view1"}, {_id: 1}),
                  {_id: "test.view1"});

        //
        // Verify that, even though a view using 4.0 query features exists, it is not possible to
        // create a new view using 4.0 query features because of feature compatibility version 3.6.
        //
        assert.commandFailedWithCode(secondaryDB.createView("view2", "coll", pipeline),
                                     ErrorCodes.QueryFeatureNotAllowed);

        MongoRunner.stopMongod(secondary);
    }

    testView([{$project: {trimmed: {$trim: {input: "  hi  "}}}}]);
    testView([{$project: {trimmed: {$ltrim: {input: "  hi  "}}}}]);
    testView([{$project: {trimmed: {$rtrim: {input: "  hi  "}}}}]);
    testView([{
        $project: {
            dateFromStringWithFormat:
                // The 'format' option was added in 4.0.
                {$dateFromString: {dateString: "2018-02-08", format: "$format"}}
        }
    }]);
    testView([{
        $project: {
            dateFromStringWithOnNull: {
                // The 'onNull' option was added in 4.0.
                $dateFromString: {dateString: "$dateString", onNull: new Date("1970-01-01")}
            }
        }
    }]);
    testView([{
        $project: {
            dateFromStringWithOnError: {
                // The 'onError' option was added in 4.0.
                $dateFromString: {dateString: "$dateString", onError: new Date("1970-01-01")}
            }
        }
    }]);
    testView([{
        $project: {
            dateToStringWithOnNull:
                // The 'onNull' option was added in 4.0.
                {$dateToString: {date: "$date", format: "%Y-%m-%d", onNull: "null input"}}
        }
    }]);
    // The 'format' option was made optional in 4.0.
    testView([{$project: {dateToStringWithoutFormat: {$dateToString: {date: "$date"}}}}]);
    testView([{$project: {conversion: {$convert: {input: "$a", to: "int"}}}}]);
    testView([{$project: {conversionWithOnNull: {$convert: {input: "$a", to: "int", onNull: 0}}}}]);

    // Test using one of the prohibited expressions inside of an $expr within a MatchExpression
    // embedded in the pipeline.
    testView([{$match: {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}}]);
    testView([{
        $graphLookup: {
            from: "foreign",
            startWith: "$start",
            connectFromField: "to",
            connectToField: "_id",
            as: "results",
            restrictSearchWithMatch: {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}
        }
    }]);
    testView([{$facet: {withinMatch: [{$match: {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}}]}}]);
    testView([{
        $facet: {
            withinGraphLookup: [{
                $graphLookup: {
                    from: "foreign",
                    startWith: "$start",
                    connectFromField: "to",
                    connectToField: "_id",
                    as: "results",
                    restrictSearchWithMatch: {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}
                }
            }]
        }
    }]);
    testView([{
        $facet: {
            withinMatch: [{$match: {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}}],
            withinGraphLookup: [{
                $graphLookup: {
                    from: "foreign",
                    startWith: "$start",
                    connectFromField: "to",
                    connectToField: "_id",
                    as: "results",
                    restrictSearchWithMatch: {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}
                }
            }]
        }
    }]);
}());
