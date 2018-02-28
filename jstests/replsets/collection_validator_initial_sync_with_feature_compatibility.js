/**
 * Test that a new replica set member can successfully sync a collection with a validator using 4.0
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
    const testName = "collection_validator_initial_sync_with_feature_compatibility";

    function testValidator(validator, nonMatchingDocument) {
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
        // Create a collection with a validator using 4.0 query features.
        //
        assert.commandWorked(testDB.createCollection("coll", {validator: validator}));

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
        // Once the new member completes its initial sync, stop it, remove it from the replica set,
        // and start it back up as an individual instance.
        //
        replTest.waitForState(secondary, [ReplSetTest.State.PRIMARY, ReplSetTest.State.SECONDARY]);

        replTest.stopSet(undefined /* send default signal */,
                         true /* don't clear data directory */);

        secondary = MongoRunner.runMongod({dbpath: secondaryDBPath, noCleanData: true});
        assert.neq(null, secondary, "mongod was unable to start up");

        //
        // Verify that the validator synced to the new member by attempting to insert a document
        // that does not validate and checking that the insert fails.
        //
        let secondaryDB = secondary.getDB("test");
        assert.writeError(secondaryDB.coll.insert(nonMatchingDocument),
                          ErrorCodes.DocumentValidationFailure);

        //
        // Verify that, even though the existing validator still works, it is not possible to create
        // a new validator using 4.0 query features because of feature compatibility version 3.6.
        //
        assert.commandFailedWithCode(
            secondaryDB.runCommand({collMod: "coll", validator: validator}),
            ErrorCodes.QueryFeatureNotAllowed);

        MongoRunner.stopMongod(secondary);
    }

    // Ban the use of expressions that were introduced or had their parsing modified in 4.0.
    testValidator({$expr: {$eq: [{$trim: {input: "$a"}}, "good"]}}, {a: "bad"});
    testValidator({$expr: {$eq: [{$ltrim: {input: "$a"}}, "good"]}}, {a: "bad"});
    testValidator({$expr: {$eq: [{$rtrim: {input: "$a"}}, "good"]}}, {a: "bad"});
    testValidator({
        $expr: {
            $eq: [
                // The 'format' option was added in 4.0.
                {$dateFromString: {dateString: "2018-02-08", format: "$format"}},
                new Date("2018-02-08")
            ]
        }
    },
                  // Swap the month and day so it doesn't match.
                  {format: "%Y-%d-%m"});
    testValidator({
        $expr: {
            $eq: [
                // The 'onNull' option was added in 4.0.
                {$dateFromString: {dateString: "$dateString", onNull: new Date("1970-01-01")}},
                new Date("2018-02-08")
            ]
        }
    },
                  {dateString: null});
    testValidator({
        $expr: {
            $eq: [
                // The 'onError' option was added in 4.0.
                {$dateFromString: {dateString: "$dateString", onError: new Date("1970-01-01")}},
                new Date("2018-02-08")
            ]
        }
    },
                  {dateString: "Not a date"});
    testValidator({
        $expr: {
            $eq: [
                // The 'onNull' option was added in 4.0.
                {$dateToString: {date: "$date", format: "%Y-%m-%d", onNull: "null input"}},
                "2018-02-08"
            ]
        }
    },
                  {date: null});
    testValidator({
        $expr: {
            $eq: [
                // The 'format' option was made optional in 4.0.
                {$dateToString: {date: "$date"}},
                "2018-02-08T00:00:00.000Z"
            ]
        }
    },
                  {date: new Date("2018-02-07")});
    testValidator({$expr: {$eq: [{$convert: {input: "$a", to: "int"}}, 2018]}}, {a: "2017"});
    testValidator({$expr: {$eq: [{$convert: {input: "$a", to: "int", onNull: 0}}, 2018]}},
                  {a: null});
    testValidator({$expr: {$eq: [{$convert: {input: "$a", to: "int", onError: 0}}, 2018]}},
                  {a: "hello"});
    testValidator({$expr: {$eq: [{$toInt: "$a"}, 2018]}}, {a: "0"});
    testValidator({$expr: {$eq: [{$toLong: "$a"}, 2018]}}, {a: 0});
    testValidator({$expr: {$eq: [{$toDouble: "$a"}, 2018]}}, {a: 0});
    testValidator({$expr: {$eq: [{$toDecimal: "$a"}, 2018]}}, {a: 0});
    testValidator({$expr: {$eq: [{$toDate: "$a"}, new ISODate("2018-02-27")]}}, {a: 0});
    testValidator({$expr: {$eq: [{$toObjectId: "$a"}, new ObjectId("aaaaaaaaaaaaaaaaaaaaaaaa")]}},
                  {a: "bbbbbbbbbbbbbbbbbbbbbbbb"});
    testValidator({$expr: {$eq: [{$toBool: "$a"}, false]}}, {a: 1});
    testValidator({$expr: {$eq: [{$toString: "$a"}, "false"]}}, {a: 1});
}());
