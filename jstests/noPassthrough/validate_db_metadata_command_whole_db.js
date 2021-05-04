/**
 * Tests the validateDBMetaData commands when running on the entire cluster.
 * @tags: [
 *   requires_sharding,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");             // For FixtureHelpers.
load("jstests/core/timeseries/libs/timeseries.js");  // For TimeseriesTest.

const dbName = jsTestName();
const collName = "coll1";

function runTest(conn) {
    const testDB = conn.getDB(dbName);
    assert.commandWorked(testDB.dropDatabase());
    const coll1 = testDB.coll1;
    const coll2 = testDB.coll2;

    function validate({dbName, coll, apiStrict, error}) {
        dbName = dbName ? dbName : null;
        coll = coll ? coll : null;
        const res = assert.commandWorked(testDB.runCommand({
            validateDBMetadata: 1,
            db: dbName,
            collection: coll,
            apiParameters: {version: "1", strict: apiStrict}
        }));

        assert(res.apiVersionErrors);
        const foundError = res.apiVersionErrors.length > 0;

        // Verify that 'apiVersionErrors' is not empty when 'error' is true, and vice versa.
        assert((!error && !foundError) || (error && foundError), res);

        if (error) {
            for (let apiError of res.apiVersionErrors) {
                assert(apiError.ns);
                if (error.code) {
                    assert.eq(apiError.code, error.code);
                }

                if (FixtureHelpers.isMongos(testDB)) {
                    // Check that every error has an additional 'shard' field on sharded clusters.
                    assert(apiError.shard);
                }
            }
        }
    }

    validate({apiStrict: true});

    //
    // Tests for indexes.
    //
    assert.commandWorked(coll1.createIndex({p: "text"}));

    validate({apiStrict: true, error: {code: ErrorCodes.APIStrictError}});

    //
    // Tests for views.
    //
    assert.commandWorked(coll1.dropIndexes());
    validate({apiStrict: true});

    // Create a view which uses unstable expression and verify that validateDBMetadata commands
    // throws an assertion.
    const viewName = "view1";
    const view = testDB.createView(
        viewName, coll2.getName(), [{$project: {v: {$_testApiVersion: {unstable: true}}}}]);

    validate({apiStrict: true, error: {code: ErrorCodes.APIStrictError}});
    validate({apiStrict: false});

    //
    // Tests for validator.
    //
    assert.commandWorked(testDB.dropDatabase());

    const validatorCollName = "validator";
    assert.commandWorked(testDB.createCollection(
        validatorCollName, {validator: {$expr: {$_testApiVersion: {unstable: true}}}}));

    validate({apiStrict: true, error: {code: ErrorCodes.APIStrictError}});

    assert.commandWorked(testDB.runCommand({drop: validatorCollName}));
}

const conn = MongoRunner.runMongod();
runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({shards: 2});
st.shardColl(dbName + "." + collName, {_id: 1}, {_id: 1});
runTest(st.s);
st.stop();
}());
