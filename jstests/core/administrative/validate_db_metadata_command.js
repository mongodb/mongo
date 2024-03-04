/**
 * Tests the validateDBMetaData commands with various input parameters.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: validateDBMetadata.
 *   not_allowed_with_signed_security_token,
 *   no_selinux,
 *   requires_timeseries,
 * # TODO SERVER-82166 remove "balancer off" and "tenant_migration_incompatible" tag once PM-2077 is
 * # over: migration can cause an index to be created before a dropIndex commits causing the
 * # metadata validation to fail
 * assumes_balancer_off,
 * tenant_migration_incompatible
 * ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const dbName = jsTestName();

const testDB = db.getSiblingDB(dbName);
assert.commandWorked(testDB.dropDatabase());
const coll1 = testDB.getCollection(jsTestName());
const coll2 = testDB.getCollection(jsTestName() + "2");

// Verify that the 'apiParameters' field is required.
const res = assert.commandFailedWithCode(testDB.runCommand({validateDBMetadata: 1}),
                                         ErrorCodes.IDLFailedToParse);

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

//
// Tests for indexes.
//
assert.commandWorked(coll1.createIndex({p: "text"}));

// All dbs but different collection name.
validate({coll: coll2.getName(), apiStrict: true});

// Different db, and collection which has unstable index should not error.
validate({dbName: "new", coll: coll1.getName(), apiStrict: true});
validate({
    dbName: "new",
    apiStrict: true,
});

// Cases where the command returns an error.
validate({apiStrict: true, error: true});
validate({coll: coll1.getName(), apiStrict: true, error: true});
validate({
    dbName: testDB.getName(),
    coll: coll1.getName(),
    apiStrict: true,
    error: {code: ErrorCodes.APIStrictError}
});
validate({dbName: testDB.getName(), apiStrict: true, error: true});

//
// Tests for views.
//
assert.commandWorked(coll1.dropIndexes());
validate({coll: coll1.getName(), apiStrict: true});

// Create a view which uses unstable expression and verify that validateDBMetadata commands throws
// an assertion.
const viewName = jsTestName() + "view1";
const view = testDB.createView(
    viewName, coll2.getName(), [{$project: {v: {$_testApiVersion: {unstable: true}}}}]);

validate({coll: viewName, apiStrict: true, error: true});
validate({dbName: dbName, apiStrict: true, error: true});

validate({dbName: "otherDB", apiStrict: true});
validate({dbName: dbName, coll: coll1.getName(), apiStrict: true});

// With view name in the input.
validate({coll: viewName, apiStrict: true, error: {code: ErrorCodes.APIStrictError}});
validate(
    {dbName: dbName, coll: viewName, apiStrict: true, error: {code: ErrorCodes.APIStrictError}});

validate({dbName: "new", coll: viewName, apiStrict: true});

// Collection named same as the view name in another db.
validate({coll: viewName, apiStrict: true, error: {code: ErrorCodes.APIStrictError}});

//
// Tests for validator.
//
assert.commandWorked(testDB.dropDatabase());

const validatorCollName = jsTestName() + "validator";
assert.commandWorked(testDB.createCollection(
    validatorCollName, {validator: {$expr: {$_testApiVersion: {unstable: true}}}}));

validate({dbName: testDB.getName(), apiStrict: true, error: true});

// Drop the collection with validation rules. By not using the 'coll.drop()' shell helper, we can
// avoid implicit collection creation in certain passthrough suites. This should increase the
// coverage of this test on sharded clusters.
assert.commandWorked(testDB.runCommand({drop: validatorCollName}));

//
// Validates the metadata across all the databases and collections after creating a time-series
// collection if time-series collection feature flag is enabled.
//
(function maybeValidateWithTimeseriesCollection() {
    const coll = "timeseriesCollectionMetaDataValidation";
    assert.commandWorked(
        testDB.createCollection(coll, {timeseries: {timeField: "time", metaField: "meta"}}));
    validate({dbName: testDB.getName(), apiStrict: true});
}());

// Clean up all the data for next run.
assert.commandWorked(testDB.dropDatabase());
