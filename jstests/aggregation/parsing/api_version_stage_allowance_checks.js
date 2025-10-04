/**
 * Tests the aggregation stages marked with 'LiteParsedDocumentSource::AllowedWithApiStrict'
 * flags.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   uses_api_parameters,
 *   requires_fcv_81,
 * ]
 */
const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
testDB.dropDatabase();

const collName = "testColl";

const testInternalClient = (function createInternalClient() {
    const connInternal = new Mongo(testDB.getMongo().host);
    const curDB = connInternal.getDB(dbName);
    assert.commandWorked(
        curDB.runCommand({
            ["hello"]: 1,
            internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
        }),
    );
    return connInternal;
})();

const curDB = testInternalClient.getDB(dbName);

// Tests that the internal stage '$mergeCursors' is allowed in requests with an internal
// client and 'apiStrict' set to true.
let result = curDB.runCommand({
    aggregate: collName,
    pipeline: [
        {
            $mergeCursors: {
                sort: {y: 1, z: 1},
                compareWholeSortKey: false,
                remotes: [],
                nss: "test.mergeCursors",
                allowPartialResults: false,
            },
        },
    ],
    cursor: {},
    writeConcern: {w: "majority"},
    readConcern: {},
    apiVersion: "1",
    apiStrict: true,
});
assert.commandWorked(result);

// Tests that the internal stage '$mergeCursors' is not allowed in user requests with default
// external client when 'apiStrict' is set to true.
result = testDB.runCommand({
    aggregate: collName,
    pipeline: [
        {
            $mergeCursors: {
                sort: {y: 1, z: 1},
                compareWholeSortKey: false,
                remotes: [],
                nss: "test.mergeCursors",
                allowPartialResults: false,
            },
        },
    ],
    cursor: {},
    writeConcern: {w: "majority"},
    apiVersion: "1",
    apiStrict: true,
});
assert.commandFailedWithCode(result, 5491300);

// Tests that the internal stage '$mergeCursors' is not allowed with default
// external client without specifying 'apiStrict' flag.
result = testDB.runCommand({
    aggregate: collName,
    pipeline: [
        {
            $mergeCursors: {
                sort: {y: 1, z: 1},
                compareWholeSortKey: false,
                remotes: [],
                nss: "test.mergeCursors",
                allowPartialResults: false,
            },
        },
    ],
    cursor: {},
    writeConcern: {w: "majority"},
    apiVersion: "1",
});
assert.commandFailedWithCode(result, 5491300);

// Tests that the 'exchange' option cannot be specified by external client with 'apiStrict' set to
// true.
result = testDB.runCommand({
    aggregate: collName,
    pipeline: [{$project: {_id: 0}}],
    cursor: {},
    writeConcern: {w: "majority"},
    apiVersion: "1",
    apiStrict: true,
    exchange: {policy: "broadcast", consumers: NumberInt(10)},
});
assert.commandFailedWithCode(result, ErrorCodes.APIStrictError);

// Tests that the 'fromRouter' option cannot be specified by external client with 'apiStrict' set to
// true.
result = testDB.runCommand({
    aggregate: collName,
    pipeline: [{$project: {_id: 0}}],
    cursor: {},
    writeConcern: {w: "majority"},
    apiVersion: "1",
    apiStrict: true,
    fromRouter: true,
});
assert.commandFailedWithCode(result, ErrorCodes.APIStrictError);

// Tests that the 'fromRouter' option should not fail by internal client with 'apiStrict' set to
// true.
result = curDB.runCommand({
    aggregate: collName,
    pipeline: [{$project: {_id: 0}}],
    cursor: {},
    writeConcern: {w: "majority"},
    readConcern: {},
    apiVersion: "1",
    apiStrict: true,
});
assert.commandWorked(result);

// Tests that the 'fromRouter' option should not fail by external client without 'apiStrict'.
result = testDB.runCommand({
    aggregate: collName,
    pipeline: [{$project: {_id: 0}}],
    cursor: {},
    writeConcern: {w: "majority"},
    apiVersion: "1",
    apiStrict: true,
});
assert.commandWorked(result);

// Tests that time-series collection can be queried (invoking $_internalUnpackBucket stage)
// from an external client with 'apiStrict'.
(function testInternalUnpackBucketAllowance() {
    const collName = "timeseriesColl";
    const timeField = "tm";
    assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: timeField}}));
    const coll = testDB[collName];
    assert.commandWorked(coll.insert({[timeField]: ISODate("2021-01-01")}));
    assert.commandWorked(
        testDB.runCommand({
            find: collName,
            apiVersion: "1",
            apiStrict: true,
        }),
    );
    assert.commandWorked(
        testDB.runCommand({
            aggregate: collName,
            pipeline: [{$match: {}}],
            cursor: {},
            apiVersion: "1",
            apiStrict: true,
        }),
    );
    const plans = [coll.find().explain(), coll.explain().aggregate([{$match: {}}])];
    assert(plans.every((plan) => plan.stages.map((x) => Object.keys(x)[0]).includes("$_internalUnpackBucket")));
})();
