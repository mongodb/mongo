// # TODO: SERVER-79909 Add multitenant support for aggregation commands using setQuerySettings -
// remove 'tenant_migration_incompatible' and 'command_not_supported_in_serverless' flags

// Tests query settings validation rules.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   tenant_migration_incompatible,
//   command_not_supported_in_serverless
// ]
//

import {QuerySettingsUtils} from "jstests/core/libs/query_settings_utils.js";

const adminDB = db.getSiblingDB("admin");
const coll = db[jsTestName()];

const utils = new QuerySettingsUtils(db, coll)

const queryA = {
    find: coll.getName(),
    $db: db.getName(),
    filter: {a: 1}
};
const querySettingsA = {
    indexHints: {allowedIndexes: ["a_1", {$natural: 1}]}
};
const nonExistentQueryShapeHash =
    "0000000000000000000000000000000000000000000000000000000000000000";

/**
 * Function used to reset the state of the DB after each test.
 */
function removeAllQuerySettings() {
    // Retrieve all querySettings hashes.
    const hashes = adminDB.aggregate([{$querySettings: {}}]).toArray().map(el => el.queryShapeHash);

    hashes.forEach(hash => {
        // Remove query settings for each hash.
        assert.commandWorked(adminDB.runCommand({removeQuerySettings: hash}));
    });

    // Check that no more querySettings exist.
    assert.eq(adminDB.aggregate([{$querySettings: {}}]).toArray().length, 0);
}

{
    // Ensure that setQuerySettings command fails for invalid input.
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: nonExistentQueryShapeHash, settings: querySettingsA}),
        7746401);
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: {notAValid: "query"}, settings: querySettingsA}),
        7746402);
    assert.commandFailedWithCode(
        db.adminCommand(
            {setQuerySettings: utils.makeQueryInstance(), settings: {notAValid: "settings"}}),
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

    assert.commandWorked(db.adminCommand({
        setQuerySettings: {
            aggregate: "order",
            $db: "someDb",
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
            "ns": { "db": "someDb", "coll": "inventory" },
            "allowedIndexes": [{ "sku": 1 }]
            }
        }
    }));
    removeAllQuerySettings();
}

{
    // Ensure that index hint may not refer to a collection which is not involved in the query.
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

    assert.commandWorked(db.adminCommand({
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
            {"indexHints": {"ns": {"db": "testDB", "coll": "order"}, "allowedIndexes": []}}
    }));
    removeAllQuerySettings();
}

{
    // Ensure that setQuerySettings command fails when multiple index hints refer to the same coll.
    assert.commandFailedWithCode(db.adminCommand({
        setQuerySettings: {find: coll.getName(), filter: {a: 123}, $db: db.getName()},
        settings: {
            "indexHints": [
                {
                    "ns": {"db": db.getName(), "coll": coll.getName()},
                    "allowedIndexes": [{"sku": 1}]
                },
                {
                    "ns": {"db": db.getName(), "coll": coll.getName()},
                    "allowedIndexes": [{"uks": 1}]
                },
            ]
        }
    }),
                                 7746608);
}
