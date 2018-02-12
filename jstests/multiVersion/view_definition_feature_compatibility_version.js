/**
 * Test that mongod will not allow creation of a view using 4.0 aggregation features when the
 * feature compatibility version is older than 4.0.
 *
 * TODO SERVER-33321: Remove FCV 3.6 validation during the 4.1 development cycle.
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    const testName = "view_definition_feature_compatibility_version_multiversion";
    const dbpath = MongoRunner.dataPath + testName;

    // In order to avoid restarting the server for each test case, we declare all the test cases up
    // front, and test them all at once.
    const pipelinesWithNewFeatures = [
        [{$project: {trimmed: {$trim: {input: "  hi  "}}}}],
        [{$project: {trimmed: {$ltrim: {input: "  hi  "}}}}],
        [{$project: {trimmed: {$rtrim: {input: "  hi  "}}}}],
        // The 'format' option was added in 4.0.
        [{
           $project: {
               dateFromStringWithFormat:
                   {$dateFromString: {dateString: "2018-02-08", format: "$format"}}
           }
        }],
        // The 'onNull' option was added in 4.0.
        [{
           $project: {
               dateFromStringWithOnNull: {
                   $dateFromString: {dateString: "$dateString", onNull: new Date("1970-01-01")}
               }
           }
        }],
        // The 'onError' option was added in 4.0.
        [{
           $project: {
               dateFromStringWithOnError: {
                   $dateFromString:
                       {dateString: "$dateString", onError: new Date("1970-01-01")}
               }
           }
        }],
        // The 'onNull' option was added in 4.0.
        [{
           $project: {
               dateToStringWithOnNull:
                   {$dateToString: {date: "$date", format: "%Y-%m-%d", onNull: "null input"}}
           }
        }],
        // The 'format' option was made optional in 4.0.
        [{$project: {dateToStringWithoutFormat: {$dateToString: {date: "$date"}}}}],
        [{$project: {conversion: {$convert: {input: "$a", to: "int"}}}}],
        [{$project: {conversionWithOnNull: {$convert: {input: "$a", to: "int", onNull: 0}}}}],
        // Test using one of the prohibited expressions inside of an $expr within a MatchExpression
        // embedded in the pipeline.
        [{$match: {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}}],
        [{
           $graphLookup: {
               from: "foreign",
               startWith: "$start",
               connectFromField: "to",
               connectToField: "_id",
               as: "results",
               restrictSearchWithMatch: {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}
           }
        }],
        [{$facet: {withinMatch: [{$match: {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}}]}}],
        [{
           $facet: {
               withinGraphLookup: [{
                   $graphLookup: {
                       from: "foreign",
                       startWith: "$start",
                       connectFromField: "to",
                       connectToField: "_id",
                       as: "results",
                       restrictSearchWithMatch:
                           {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}
                   }
               }]
           }
        }],
        [{
           $facet: {
               withinMatch: [{$match: {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}}],
               withinGraphLookup: [{
                   $graphLookup: {
                       from: "foreign",
                       startWith: "$start",
                       connectFromField: "to",
                       connectToField: "_id",
                       as: "results",
                       restrictSearchWithMatch:
                           {$expr: {$eq: [{$trim: {input: "$a"}}, "hi"]}}
                   }
               }]
           }
        }]
    ];

    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
    assert.neq(null, conn, "mongod was unable to start up");
    let testDB = conn.getDB(testName);

    // Explicitly set feature compatibility version 4.0.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    // Test that we are able to create a new view with any of the new features.
    pipelinesWithNewFeatures.forEach(
        (pipe, i) => assert.commandWorked(
            testDB.createView("firstView" + i, "coll", pipe),
            `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV 4.0`));

    // Test that we are able to create a new view with any of the new features.
    pipelinesWithNewFeatures.forEach(function(pipe, i) {
        assert(testDB["firstView" + i].drop(), `Drop of view with pipeline ${tojson(pipe)} failed`);
        assert.commandWorked(testDB.createView("firstView" + i, "coll", []));
        assert.commandWorked(
            testDB.runCommand({collMod: "firstView" + i, viewOn: "coll", pipeline: pipe}),
            `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV 4.0`);
    });

    // Create an empty view which we will attempt to update to use 4.0 query features under
    // feature compatibility mode 3.6.
    assert.commandWorked(testDB.createView("emptyView", "coll", []));

    // Set the feature compatibility version to 3.6.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

    // Read against an existing view using 4.0 query features should not fail.
    pipelinesWithNewFeatures.forEach(
        (pipe, i) => assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                                          `Failed to query view with pipeline ${tojson(pipe)}`));

    // Trying to create a new view using 4.0 query features should fail.
    pipelinesWithNewFeatures.forEach(
        (pipe, i) => assert.commandFailedWithCode(
            testDB.createView("view_fail" + i, "coll", pipe),
            ErrorCodes.QueryFeatureNotAllowed,
            `Expected *not* to be able to create view with pipeline ${tojson(pipe)} while in FCV 3.6`));

    // Trying to update existing view to use 4.0 query features should also fail.
    pipelinesWithNewFeatures.forEach(
        (pipe, i) => assert.commandFailedWithCode(
            testDB.runCommand({collMod: "emptyView", viewOn: "coll", pipeline: pipe}),
            ErrorCodes.QueryFeatureNotAllowed,
            `Expected *not* to be able to modify view to use pipeline ${tojson(pipe)} while in FCV 3.6`));

    MongoRunner.stopMongod(conn);

    // Starting up a 3.6 mongod with 4.0 query features will succeed.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.6", noCleanData: true});
    assert.neq(null, conn, "mongod 3.6 was unable to start up");
    testDB = conn.getDB(testName);

    // Reads will fail against views with 4.0 query features when running a 3.6 binary.
    // Not checking the code returned on failure as it is not uniform across the various
    // 'pipeline' arguments tested.
    pipelinesWithNewFeatures.forEach(
        (pipe, i) => assert.commandFailed(
            testDB.runCommand({find: "firstView" + i}),
            `Expected read against view with pipeline ${tojson(pipe)} to fail on 3.6 binary`));

    // Test that a read against a view that does not contain 4.0 query features succeeds.
    assert.commandWorked(testDB.runCommand({find: "emptyView"}));

    MongoRunner.stopMongod(conn);

    // Starting up a 4.0 mongod should succeed, even though the feature compatibility version is
    // still set to 3.6.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");
    testDB = conn.getDB(testName);

    // Read against an existing view using 4.0 query features should not fail.
    pipelinesWithNewFeatures.forEach(
        (pipe, i) => assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                                          `Failed to query view with pipeline ${tojson(pipe)}`));

    // Set the feature compatibility version back to 4.0.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    pipelinesWithNewFeatures.forEach(function(pipe, i) {
        assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                             `Failed to query view with pipeline ${tojson(pipe)}`);
        // Test that we are able to create a new view with any of the new features.
        assert.commandWorked(
            testDB.createView("secondView" + i, "coll", pipe),
            `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV 4.0`);

        // Test that we are able to update an existing view to use any of the new features.
        assert(testDB["secondView" + i].drop(),
               `Drop of view with pipeline ${tojson(pipe)} failed`);
        assert.commandWorked(testDB.createView("secondView" + i, "coll", []));
        assert.commandWorked(
            testDB.runCommand({collMod: "secondView" + i, viewOn: "coll", pipeline: pipe}),
            `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV 4.0`);
    });

    // Set the feature compatibility version to 3.6 and then restart with
    // internalValidateFeaturesAsMaster=false.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({
        dbpath: dbpath,
        binVersion: "latest",
        noCleanData: true,
        setParameter: "internalValidateFeaturesAsMaster=false"
    });
    assert.neq(null, conn, "mongod was unable to start up");
    testDB = conn.getDB(testName);

    pipelinesWithNewFeatures.forEach(function(pipe, i) {
        // Even though the feature compatibility version is 3.6, we should still be able to create a
        // view using 4.0 query features, because internalValidateFeaturesAsMaster is false.
        assert.commandWorked(
            testDB.createView("thirdView" + i, "coll", pipe),
            `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV 3.6 ` +
                `with internalValidateFeaturesAsMaster=false`);

        // We should also be able to modify a view to use 4.0 query features.
        assert(testDB["thirdView" + i].drop(), `Drop of view with pipeline ${tojson(pipe)} failed`);
        assert.commandWorked(testDB.createView("thirdView" + i, "coll", []));
        assert.commandWorked(
            testDB.runCommand({collMod: "thirdView" + i, viewOn: "coll", pipeline: pipe}),
            `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV 3.6 ` +
                `with internalValidateFeaturesAsMaster=false`);
    });

    MongoRunner.stopMongod(conn);

    // Starting up a 3.6 mongod with 4.0 query features.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.6", noCleanData: true});
    assert.neq(null, conn, "mongod 3.6 was unable to start up");
    testDB = conn.getDB(testName);

    // Existing views with 4.0 query features can be dropped.
    pipelinesWithNewFeatures.forEach(
        (pipe, i) => assert(testDB["firstView" + i].drop(),
                            `Drop of view with pipeline ${tojson(pipe)} failed`));
    assert(testDB.system.views.drop(), "Drop of system.views collection failed");

    MongoRunner.stopMongod(conn);
}());
