// Tests query settings validation rules.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   simulate_atlas_proxy_incompatible,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js"
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const collName = jsTestName();
assertDropAndRecreateCollection(db, collName);

const qsutils = new QuerySettingsUtils(db, collName)

const querySettingsA = {
    indexHints: {ns: {db: db.getName(), coll: collName}, allowedIndexes: ["a_1", {$natural: 1}]}
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
    assert.commandFailedWithCode(db.adminCommand({
        setQuerySettings: qsutils.makeFindQueryInstance(),
        settings: {indexHints: {allowedIndexes: ["a_1"]}}
    }),
                                 40414);
    assert.commandFailedWithCode(db.adminCommand({
        setQuerySettings: qsutils.makeFindQueryInstance(),
        settings: {indexHints: {ns: {db: db.getName()}, allowedIndexes: ["a_1"]}}
    }),
                                 8727501);
    assert.commandFailedWithCode(db.adminCommand({
        setQuerySettings: qsutils.makeFindQueryInstance(),
        settings: {indexHints: {ns: {coll: collName}, allowedIndexes: ["a_1"]}}
    }),
                                 8727500);
}

{
    // Ensure that removeQuerySettings command ignores for invalid input.
    assert.commandWorked(db.adminCommand({removeQuerySettings: nonExistentQueryShapeHash}));
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
    const query = qsutils.makeFindQueryInstance({filter: {a: 15}});
    assert.commandFailedWithCode(db.adminCommand({setQuerySettings: query, settings: {}}), 7746604);

    // Insert some settings.
    assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: {reject: true}}));
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration({reject: true}, query)]);

    // Ensure updating with empty query settings fails.
    const queryShapeHash = qsutils.getQueryHashFromQuerySettings(query);
    assert.commandFailedWithCode(db.adminCommand({setQuerySettings: queryShapeHash, settings: {}}),
                                 8727502);
    assert.commandFailedWithCode(db.adminCommand({setQuerySettings: query, settings: {}}), 8727503);
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration({reject: true}, query)]);

    // Clean-up after the end of the test.
    qsutils.removeAllQuerySettings();
}
