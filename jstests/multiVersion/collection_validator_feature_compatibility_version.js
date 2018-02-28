/**
 * Test that mongod will not allow creation of collection validators using 4.0 query features when
 * the feature compatibility version is older than 4.0.
 *
 * TODO SERVER-33321: Remove FCV 3.6 validation during the 4.1 development cycle.
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    const testName = "collection_validator_feature_compatibility_version";
    const dbpath = MongoRunner.dataPath + testName;

    // In order to avoid restarting the server for each test case, we declare all the test cases up
    // front, and test them all at once.
    const testCases = [
        {
          validator: {$expr: {$eq: [{$trim: {input: "$a"}}, "good"]}},
          nonMatchingDocument: {a: "bad"}
        },
        {
          validator: {$expr: {$eq: [{$ltrim: {input: "$a"}}, "good"]}},
          nonMatchingDocument: {a: "bad"}
        },
        {
          validator: {$expr: {$eq: [{$rtrim: {input: "$a"}}, "good"]}},
          nonMatchingDocument: {a: "bad"}
        },
        {
          validator: {
              $expr: {
                  $eq: [
                      // The 'format' option was added in 4.0.
                      {$dateFromString: {dateString: "2018-02-08", format: "$format"}},
                      new Date("2018-02-08")
                  ]
              }
          },
          // Swap the month and day so it doesn't match.
          nonMatchingDocument: {format: "%Y-%d-%m"}
        },
        {
          validator: {
              $expr: {
                  $eq: [
                      // The 'onNull' option was added in 4.0.
                      {
                        $dateFromString:
                            {dateString: "$dateString", onNull: new Date("1970-01-01")}
                      },
                      new Date("2018-02-08")
                  ]
              }
          },
          nonMatchingDocument: {dateString: null}
        },
        {
          validator: {
              $expr: {
                  $eq: [
                      // The 'onError' option was added in 4.0.
                      {
                        $dateFromString:
                            {dateString: "$dateString", onError: new Date("1970-01-01")}
                      },
                      new Date("2018-02-08")
                  ]
              }
          },
          nonMatchingDocument: {dateString: "Not a date"}
        },
        {
          validator: {
              $expr: {
                  $eq: [
                      // The 'onNull' option was added in 4.0.
                      {$dateToString: {date: "$date", format: "%Y-%m-%d", onNull: "null input"}},
                      "2018-02-08"
                  ]
              }
          },
          nonMatchingDocument: {date: null}
        },
        {
          validator: {
              $expr: {
                  $eq: [
                      // The 'format' option was made optional in 4.0.
                      {$dateToString: {date: "$date"}},
                      "2018-02-08T00:00:00.000Z"
                  ]
              }
          },
          nonMatchingDocument: {date: new Date("2018-02-07")}
        },
        {
          validator: {$expr: {$eq: [{$convert: {input: "$a", to: "int"}}, 2018]}},
          nonMatchingDocument: {a: "2017"}
        },
        {
          validator: {$expr: {$eq: [{$convert: {input: "$a", to: "int", onNull: 0}}, 2018]}},
          nonMatchingDocument: {a: null}
        },
        {
          validator: {$expr: {$eq: [{$convert: {input: "$a", to: "int", onError: 0}}, 2018]}},
          nonMatchingDocument: {a: "hello"}
        },
        {validator: {$expr: {$eq: [{$toInt: "$a"}, 2018]}}, nonMatchingDocument: {a: "0"}},
        {validator: {$expr: {$eq: [{$toLong: "$a"}, 2018]}}, nonMatchingDocument: {a: 0}},
        {validator: {$expr: {$eq: [{$toDouble: "$a"}, 2018]}}, nonMatchingDocument: {a: 0}},
        {validator: {$expr: {$eq: [{$toDecimal: "$a"}, 2018]}}, nonMatchingDocument: {a: 0}},
        {
          validator: {$expr: {$eq: [{$toDate: "$a"}, new ISODate("2018-02-27")]}},
          nonMatchingDocument: {a: 0}
        },
        {
          validator:
              {$expr: {$eq: [{$toObjectId: "$a"}, new ObjectId("aaaaaaaaaaaaaaaaaaaaaaaa")]}},
          nonMatchingDocument: {a: "bbbbbbbbbbbbbbbbbbbbbbbb"}
        },
        {validator: {$expr: {$eq: [{$toBool: "$a"}, false]}}, nonMatchingDocument: {a: 1}},
        {validator: {$expr: {$eq: [{$toString: "$a"}, "false"]}}, nonMatchingDocument: {a: 1}},
    ];

    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
    assert.neq(null, conn, "mongod was unable to start up");

    let testDB = conn.getDB(testName);

    let adminDB = conn.getDB("admin");

    // Explicitly set feature compatibility version 4.0.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "4.0"}));

    testCases.forEach(function(test, i) {
        // Create a collection with a validator using 4.0 query features.
        const coll = testDB["coll" + i];
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {validator: test.validator}),
            `Expected to be able to create collection with validator ${tojson(test.validator)}`);

        // The validator should cause this insert to fail.
        assert.writeErrorWithCode(
            coll.insert(test.nonMatchingDocument),
            ErrorCodes.DocumentValidationFailure,
            `Expected document ${tojson(test.nonMatchingDocument)} to fail validation for ` +
                `collection with validator ${tojson(test.validator)}`);

        // Set a validator using 4.0 query features on an existing collection.
        coll.drop();
        assert.commandWorked(testDB.createCollection(coll.getName()));
        assert.commandWorked(
            testDB.runCommand({collMod: coll.getName(), validator: test.validator}),
            `Expected to be able to modify collection validator to be ${tojson(test.validator)}`);

        // Another failing update.
        assert.writeErrorWithCode(
            coll.insert(test.nonMatchingDocument),
            ErrorCodes.DocumentValidationFailure,
            `Expected document ${tojson(test.nonMatchingDocument)} to fail validation for ` +
                `collection with validator ${tojson(test.validator)}`);
    });

    // Set the feature compatibility version to 3.6.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

    testCases.forEach(function(test, i) {
        // The validator is already in place, so it should still cause this insert to fail.
        const coll = testDB["coll" + i];
        assert.writeErrorWithCode(
            coll.insert(test.nonMatchingDocument),
            ErrorCodes.DocumentValidationFailure,
            `Expected document ${tojson(test.nonMatchingDocument)} to fail validation for ` +
                `collection with validator ${tojson(test.validator)}`);

        // Trying to create a new collection with a validator using 4.0 query features should fail
        // while feature compatibility version is 3.6.
        let res = testDB.createCollection("other", {validator: test.validator});
        assert.commandFailedWithCode(
            res,
            ErrorCodes.QueryFeatureNotAllowed,
            `Expected *not* to be able to create collection with validator ${tojson(test.validator)}`);
        assert(
            res.errmsg.match(/feature compatibility version/),
            `Expected error message from createCollection with validator ` +
                `${tojson(test.validator)} to reference 'feature compatibility version' but got: ` +
                res.errmsg);

        // Trying to update a collection with a validator using 4.0 query features should also fail.
        res = testDB.runCommand({collMod: coll.getName(), validator: test.validator});
        assert.commandFailedWithCode(
            res,
            ErrorCodes.QueryFeatureNotAllowed,
            `Expected to be able to create collection with validator ${tojson(test.validator)}`);
        assert(
            res.errmsg.match(/feature compatibility version/),
            `Expected error message from createCollection with validator ` +
                `${tojson(test.validator)} to reference 'feature compatibility version' but got: ` +
                res.errmsg);
    });

    MongoRunner.stopMongod(conn);

    // If we try to start up a 3.6 mongod, it will fail, because it will not be able to parse
    // the validator using 4.0 query features.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.6", noCleanData: true});
    assert.eq(
        null, conn, "mongod 3.6 started, even with a validator using 4.0 query features in place.");

    // Starting up a 4.0 mongod, however, should succeed, even though the feature compatibility
    // version is still set to 3.6.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");

    adminDB = conn.getDB("admin");
    testDB = conn.getDB(testName);

    // And the validator should still work.
    testCases.forEach(function(test, i) {
        const coll = testDB["coll" + i];
        assert.writeErrorWithCode(
            coll.insert(test.nonMatchingDocument),
            ErrorCodes.DocumentValidationFailure,
            `Expected document ${tojson(test.nonMatchingDocument)} to fail validation for ` +
                `collection with validator ${tojson(test.validator)}`);

        // Remove the validator.
        assert.commandWorked(testDB.runCommand({collMod: coll.getName(), validator: {}}));
    });

    MongoRunner.stopMongod(conn);

    // Now, we should be able to start up a 3.6 mongod.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.6", noCleanData: true});
    assert.neq(
        null,
        conn,
        "mongod 3.6 failed to start, even after we removed the validator using 4.0 query features");

    MongoRunner.stopMongod(conn);

    // The rest of the test uses mongod 4.0.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");

    adminDB = conn.getDB("admin");
    testDB = conn.getDB(testName);

    // Set the feature compatibility version back to 4.0.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "4.0"}));

    testCases.forEach(function(test, i) {
        const coll = testDB["coll2" + i];

        // Now we should be able to create a collection with a validator using 4.0 query features
        // again.
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {validator: test.validator}),
            `Expected to be able to create collection with validator ${tojson(test.validator)}`);

        // And we should be able to modify a collection to have a validator using 4.0 query
        // features.
        assert.commandWorked(
            testDB.runCommand({collMod: coll.getName(), validator: test.validator}),
            `Expected to be able to modify collection validator to be ${tojson(test.validator)}`);
    });

    // Set the feature compatibility version to 3.6 and then restart with
    // internalValidateFeaturesAsMaster=false.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({
        dbpath: dbpath,
        binVersion: "latest",
        noCleanData: true,
        setParameter: "internalValidateFeaturesAsMaster=false"
    });
    assert.neq(null, conn, "mongod was unable to start up");

    testDB = conn.getDB(testName);

    testCases.forEach(function(test, i) {
        const coll = testDB["coll3" + i];
        // Even though the feature compatibility version is 3.6, we should still be able to add a
        // validator using 4.0 query features, because internalValidateFeaturesAsMaster is false.
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {validator: test.validator}),
            `Expected to be able to create collection with validator ${tojson(test.validator)}`);

        // We should also be able to modify a collection to have a validator using 4.0 query
        // features.
        coll.drop();
        assert.commandWorked(testDB.createCollection(coll.getName()));
        assert.commandWorked(
            testDB.runCommand({collMod: coll.getName(), validator: test.validator}),
            `Expected to be able to modify collection validator to be ${tojson(test.validator)}`);
    });

    MongoRunner.stopMongod(conn);

}());
