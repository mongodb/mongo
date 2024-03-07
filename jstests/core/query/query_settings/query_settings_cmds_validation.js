// Tests query settings validation rules.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   simulate_atlas_proxy_incompatible,
//   requires_fcv_80,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js"
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const collName = jsTestName();

const qsutils = new QuerySettingsUtils(db, collName)

const querySettingsA = {
    indexHints: {allowedIndexes: ["a_1", {$natural: 1}]}
};
const nonExistentQueryShapeHash = "0".repeat(64);

{
    // Ensure that setQuerySettings command fails for invalid input.
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: {notAValid: "query"}, settings: querySettingsA}),
        7746402);
    assert.commandFailedWithCode(
        db.adminCommand(
            {setQuerySettings: qsutils.makeFindQueryInstance(), settings: {notAValid: "settings"}}),
        40415);
}

{
    // Ensure that removeQuerySettings command fails for invalid input.
    assert.commandFailedWithCode(db.adminCommand({removeQuerySettings: nonExistentQueryShapeHash}),
                                 7746701);
    assert.commandFailedWithCode(db.adminCommand({removeQuerySettings: {notAValid: "query"}}),
                                 7746402);
}

{
    // Ensure that $querySettings agg stage inherits the constraints from the underlying alias
    // stages, including $queue.
    assert.commandFailedWithCode(
        db.adminCommand(
            {aggregate: 1, pipeline: [{$documents: []}, {$querySettings: {}}], cursor: {}}),
        40602);
}

{
    // Ensure that setQuerySettings command fails when there are more than one collection in the
    // input query and namespaces are not explicitly given.
    assertDropAndRecreateCollection(db, "order");
    assert.commandFailedWithCode(
            db.adminCommand({
                setQuerySettings: {
                  aggregate: "order",
                  $db: db.getName(),
                  pipeline: [{
                    $lookup: {
                      from: "inventory",
                      localField: "item",
                      foreignField: "sku",
                      as: "inventory_docs"
                    }
                  }]
                },
                settings: {
                  "indexHints": {
                    "allowedIndexes": [{ "sku": 1 }]
                  }
                }
              }
              ), 7746602);

    const queryInstance = {
        aggregate: "order",
        $db: db.getName(),
        pipeline: [{
            $lookup:
                {from: "inventory", localField: "item", foreignField: "sku", as: "inventory_docs"}
        }]
    };
    const settings = {
        "indexHints":
            {"ns": {"db": db.getName(), "coll": "inventory"}, "allowedIndexes": [{"sku": 1}]}
    };
    assert.commandWorked(db.adminCommand({setQuerySettings: queryInstance, settings: settings}));
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(settings, queryInstance)]);
    qsutils.removeAllQuerySettings();
}

{
    // Ensure that index hint may not refer to a collection which is not involved in the query.
    assertDropAndRecreateCollection(db, "order");
    assert.commandFailedWithCode(db.adminCommand({
        setQuerySettings: {
            aggregate: "order",
            $db: "testDB",
            pipeline: [{
            $lookup: {
                from: "inventory",
                localField: "item",
                foreignField: "sku",
                as: "inventory_docs"
            }
            }]
        },
        settings:
            {"indexHints": {"ns": {"db": "testDB", "coll": "someOtherColl"}, "allowedIndexes": []}}
    }),
                                 7746603);

    const queryInstance = {
        aggregate: "order",
        $db: db.getName(),
        pipeline: [{
            $lookup:
                {from: "inventory", localField: "item", foreignField: "sku", as: "inventory_docs"}
        }]
    };
    const settings = {
        "indexHints": {"ns": {"db": db.getName(), "coll": "order"}, "allowedIndexes": []}
    };
    assert.commandWorked(db.adminCommand({setQuerySettings: queryInstance, settings: settings}));
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(settings, queryInstance)]);
    qsutils.removeAllQuerySettings();
}

{
    // Ensure that setQuerySettings command fails when multiple index hints refer to the same coll.
    assert.commandFailedWithCode(db.adminCommand({
        setQuerySettings: {find: collName, filter: {a: 123}, $db: db.getName()},
        settings: {
            "indexHints": [
                {"ns": {"db": db.getName(), "coll": collName}, "allowedIndexes": [{"sku": 1}]},
                {"ns": {"db": db.getName(), "coll": collName}, "allowedIndexes": [{"uks": 1}]},
            ]
        }
    }),
                                 7746608);
}

{
    // Ensure setQuerySettings fails for internal dbs.
    function testInternalDBQuerySettings(dbName, collection, index) {
        // Ensure that setQuerySettings command fails for this db.
        assert.commandFailedWithCode(db.adminCommand({
            setQuerySettings: {find: collection, $db: dbName},
            settings: {
                "indexHints": [
                    {"ns": {"db": dbName, "coll": collection}, "allowedIndexes": [index]},
                ]
            }
        }),
                                     8584900);
    }
    testInternalDBQuerySettings("admin", "system.version", {version: 1});
    testInternalDBQuerySettings("local", "system.views", {viewOn: 1});
    testInternalDBQuerySettings("config", "clusterParameters", {clusterParameterTime: 1});
}

{
    // Ensure setQuerySettings fails for system collections.
    assert.commandFailedWithCode(db.adminCommand({
        setQuerySettings: {find: "system.foobar", $db: db.getName()},
        settings: {
            "indexHints": [
                {
                    "ns": {"db": db.getName(), "coll": "system.foobar"},
                    "allowedIndexes": [{"anything": 1}]
                },
            ]
        }
    }),
                                 8584901);
}

{
    // Ensure that inserting empty settings fails.
    let query = qsutils.makeFindQueryInstance({filter: {a: 15}});
    assert.commandFailedWithCode(db.adminCommand({setQuerySettings: query, settings: {}}), 7746604);

    // Insert some settings.
    assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: {reject: true}}));

    // Ensure that updating with empty settings fails.
    assert.commandFailedWithCode(db.adminCommand({setQuerySettings: query, settings: {}}), 7746604);

    // Clean-up after the end of the test.
    qsutils.removeAllQuerySettings();
}
