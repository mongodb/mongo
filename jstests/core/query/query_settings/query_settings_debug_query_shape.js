// Tests the 'showDebugQueryShape' option for the '$querySettings' aggregation stage.
// @tags: [
//   # TODO SERVER-98659 Investigate why this test is failing on
//   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
//   does_not_support_stepdowns,
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const collName = jsTestName();
const qsutils = new QuerySettingsUtils(db, collName);
qsutils.removeAllQuerySettings();

const settings = {
    queryFramework: "classic",
};

// Creating the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
assertDropAndRecreateCollection(db, collName);

function runTest({queryInstance, expectedDebugQueryShape, shouldRunExplain = true}) {
    // Ensure that no query settings are present at the start of the test.
    qsutils.assertQueryShapeConfiguration([]);

    // Set some settings for the desired query instance, so we can later query for the debug query
    // shape.
    assert.commandWorked(db.adminCommand({setQuerySettings: queryInstance, settings}));
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(settings, queryInstance)],
        shouldRunExplain,
    );

    // Compare the actual debug query shape against the expected one. Using 'assert.docEq()' has the
    // added bonus of ensuring that the 'tenantId' does not get leaked within the 'cmdNs' property.
    // We need to wrap the query shape comparison into an 'assert.soon()' call here because during
    // FCV donwgrade the collection with representative query shapes is discarded, and there is a
    // small window of time in which there is no representative query present, and thus no debug
    // query shape can be computed from it by the $querySettings command.
    assert.soon(() => {
        const actualDebugQueryShape = qsutils.getQuerySettings({
            showDebugQueryShape: true,
        })[0].debugQueryShape;
        if (actualDebugQueryShape === undefined) {
            return false;
        }
        assert.docEq(expectedDebugQueryShape, actualDebugQueryShape);
        return true;
    }, "expecting debugQueryShape to be as expected");

    // Remove the newly added query settings, so the rest of the tests can be executed from a fresh
    // state.
    qsutils.removeAllQuerySettings();
}

// Test the find command case.
runTest({
    queryInstance: qsutils.makeFindQueryInstance({filter: {evil: true}}),
    expectedDebugQueryShape: {
        cmdNs: {db: db.getName(), coll: collName},
        command: "find",
        filter: {evil: {$eq: "?bool"}},
    },
});

// Test the aggregate command case.
runTest({
    queryInstance: qsutils.makeAggregateQueryInstance({
        pipeline: [
            {
                $lookup: {
                    from: "inventory",
                    localField: "item",
                    foreignField: "sku",
                    as: "inventory_docs",
                },
            },
            {
                $match: {
                    qty: {$lt: 5},
                    manufacturer: {
                        $in: ["Acme Corporation", "Umbrella Corporation"],
                    },
                },
            },
            {
                $count: "itemsLowOnStock",
            },
        ],
    }),
    expectedDebugQueryShape: {
        cmdNs: {db: db.getName(), coll: collName},
        command: "aggregate",
        pipeline: [
            {
                $lookup: {
                    from: "inventory",
                    as: "inventory_docs",
                    localField: "item",
                    foreignField: "sku",
                },
            },
            {
                $match: {
                    $and: [
                        {
                            qty: {
                                $lt: "?number",
                            },
                        },
                        {
                            manufacturer: {
                                $in: "?array<?string>",
                            },
                        },
                    ],
                },
            },
            {
                $group: {
                    _id: "?null",
                    itemsLowOnStock: {
                        $sum: "?number",
                    },
                },
            },
            {
                $project: {
                    itemsLowOnStock: true,
                    _id: false,
                },
            },
        ],
    },
});

// Test the inception case: setting query settings on '$querySettings'.
runTest({
    queryInstance: qsutils.makeAggregateQueryInstance(
        {
            pipeline: [{$querySettings: {showDebugQueryShape: true}}],
        },
        /* collectionless */ true,
    ),
    expectedDebugQueryShape: {
        cmdNs: {db: db.getName(), coll: "$cmd.aggregate"},
        command: "aggregate",
        pipeline: [{$querySettings: {showDebugQueryShape: true}}],
    },
    // Since it's a collectionless aggregate, the explain does not contain the 'queryPlanner' field.
    shouldRunExplain: false,
});
