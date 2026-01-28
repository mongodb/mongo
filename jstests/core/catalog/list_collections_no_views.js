/**
 * SERVER-25942 Test that views are not validated in the case that only collections are queried.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: applyOps.
 *   not_allowed_with_signed_security_token,
 *   assumes_against_mongod_not_mongos,
 *   assumes_superuser_permissions,
 *   # applyOps is not retryable.
 *   requires_non_retryable_commands,
 *   # Antithesis can inject a fault while an invalid view still exists, which causes validation
 *   # failures in hooks, as they leave the database in a broken state where listCollections fails.
 *   antithesis_incompatible,
 *   requires_timeseries,
 * ]
 */
import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {isFCVgte} from "jstests/libs/feature_compatibility_version.js";

let mydb = db.getSiblingDB("list_collections_no_views");

assert.commandWorked(mydb.createCollection("foo"));
assert.commandWorked(mydb.createView("bar", "foo", []));
assert.commandWorked(mydb.createCollection("ts", {timeseries: {timeField: "t", metaField: "m"}}));

// Helper to create base expected array with regular collections (no views, no timeseries)
function getBaseCollectionsExpected() {
    return [
        {
            "name": "foo",
            "type": "collection",
        },
        {
            "name": "system.views",
            "type": "collection",
        },
    ];
}

// Helper to add timeseries-related collections to expected array
function addTimeseriesExpected(expectedArray, shouldIncludeTimeseriesType) {
    const viewlessTimeseriesEnabled = areViewlessTimeseriesEnabled(mydb);
    // In legacy mode, bucket is always present as a separate collection
    if (!viewlessTimeseriesEnabled) {
        expectedArray.push({
            "name": "system.buckets.ts",
            "type": "collection",
        });
    }
    // Main namespace is included when filter allows type "timeseries"
    if (shouldIncludeTimeseriesType) {
        expectedArray.push({
            "name": "ts",
            "type": "timeseries",
        });
    }
}

// Helper to sort collection objects by name
function sortCollectionsByName(c1, c2) {
    if (c1.name > c2.name) {
        return 1;
    }
    if (c1.name < c2.name) {
        return -1;
    }
    return 0;
}

let all = mydb.runCommand({listCollections: 1});
assert.commandWorked(all);

let allExpected = getBaseCollectionsExpected();
allExpected.push({
    "name": "bar",
    "type": "view",
});

// Add timeseries to expected results
// listCollections without filter returns all: collections, views, and timeseries
addTimeseriesExpected(allExpected, true /* shouldIncludeTimeseriesType */);

assert.eq(
    allExpected.sort(sortCollectionsByName),
    all.cursor.firstBatch
        .map(function (c) {
            return {name: c.name, type: c.type};
        })
        .sort(sortCollectionsByName),
);

// TODO (SERVER-25493): {type: {$exists: false}} is needed for versions <= 3.2
let collOnlyCommand = {
    listCollections: 1,
    filter: {$or: [{type: "collection"}, {type: {$exists: false}}]},
};

let collOnly = mydb.runCommand(collOnlyCommand);
assert.commandWorked(collOnly);

let collOnlyExpected = [
    {
        "name": "foo",
        "type": "collection",
    },
    {
        "name": "system.views",
        "type": "collection",
    },
];

// Filter {$or: [{type: "collection"}, {type: {$exists: false}}]} excludes type="timeseries"
addTimeseriesExpected(collOnlyExpected, false /* shouldIncludeTimeseriesType */);

assert.eq(
    collOnlyExpected.sort(sortCollectionsByName),
    collOnly.cursor.firstBatch
        .map(function (c) {
            return {name: c.name, type: c.type};
        })
        .sort(sortCollectionsByName),
);

let viewOnly = mydb.runCommand({listCollections: 1, filter: {type: "view"}});
assert.commandWorked(viewOnly);
let viewOnlyExpected = [
    {
        "name": "bar",
        "type": "view",
    },
];

assert.eq(
    viewOnlyExpected,
    viewOnly.cursor.firstBatch
        .map(function (c) {
            return {name: c.name, type: c.type};
        })
        .sort(sortCollectionsByName),
);

assert.commandWorked(
    db.adminCommand({
        applyOps: [
            {
                op: "i",
                ns: mydb.getName() + ".system.views",
                o: {_id: "invalid_view_def", invalid: NumberLong(1000)},
            },
        ],
    }),
);

let collOnlyInvalidView = mydb.runCommand(collOnlyCommand);
assert.eq(
    collOnlyExpected,
    collOnlyInvalidView.cursor.firstBatch
        .map(function (c) {
            return {name: c.name, type: c.type};
        })
        .sort(sortCollectionsByName),
);

// Test the {type: {$ne: "view"}} filter also skips loading the view
// catalog while including timeseries collections
// This test only runs on binary versions >= 8.3
if (isFCVgte(mydb, "8.3")) {
    jsTest.log.info('Testing filter {type: {$ne: "view"}}');
    let excludeViewsCommand = {
        listCollections: 1,
        filter: {type: {$ne: "view"}},
    };

    let excludeViewsResult = mydb.runCommand(excludeViewsCommand);
    assert.commandWorked(excludeViewsResult);

    // Filter {type: {$ne: "view"}} should include type="timeseries"
    let excludeViewsExpected = getBaseCollectionsExpected();
    addTimeseriesExpected(excludeViewsExpected, true /* shouldIncludeTimeseriesType */);

    assert.eq(
        excludeViewsExpected.sort(sortCollectionsByName),
        excludeViewsResult.cursor.firstBatch
            .map(function (c) {
                return {name: c.name, type: c.type};
            })
            .sort(sortCollectionsByName),
    );
}

assert.commandFailed(mydb.runCommand({listCollections: 1}));
assert.commandFailed(mydb.runCommand({listCollections: 1, filter: {type: "view"}}));

// Clean up the invalid view that was created for testing
assert.commandWorked(
    db.adminCommand({
        applyOps: [
            {
                op: "d",
                ns: mydb.getName() + ".system.views",
                o: {_id: "invalid_view_def"},
            },
        ],
    }),
);

// Fix database state for end of test validation and burn-in tests
mydb.dropDatabase();
