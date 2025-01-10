// Tests query settings validation rules.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   requires_fcv_80,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const collName = jsTestName();
assertDropAndRecreateCollection(db, collName);

const qsutils = new QuerySettingsUtils(db, collName);
qsutils.removeAllQuerySettings();

const querySettingsA = {
    indexHints: {ns: {db: db.getName(), coll: collName}, allowedIndexes: ["a_1", {$natural: 1}]}
};
const nonExistentQueryShapeHash = "0".repeat(64);

{
    // Ensure that setQuerySettings command fails for invalid input.
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: {notAValid: "query"}, settings: querySettingsA}),
        7746402);
    assert.commandFailedWithCode(db.adminCommand({
        setQuerySettings: qsutils.makeFindQueryInstance(),
        settings: {indexHints: {allowedIndexes: ["a_1"]}}
    }),
                                 ErrorCodes.IDLFailedToParse);
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
    assert.commandFailedWithCode(db.adminCommand({setQuerySettings: query, settings: {}}), 8727502);

    // Ensure that inserting settings that only have the 'comment' field fails.
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: query, settings: {comment: "hello!"}}), 7746604);

    // Insert some settings.
    assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: {reject: true}}));
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration({reject: true}, query)]);

    // Ensure updating with empty query settings fails.
    const queryShapeHash = qsutils.getQueryShapeHashFromQuerySettings(query);
    assert.commandFailedWithCode(db.adminCommand({setQuerySettings: queryShapeHash, settings: {}}),
                                 8727502);
    assert.commandFailedWithCode(db.adminCommand({setQuerySettings: query, settings: {}}), 8727502);
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration({reject: true}, query)]);

    // Ensure that updating settings that only have the 'comment' field fails.
    // Note: if only the comment is changed, the command does not fail since 'reject' is set to
    // true, which makes the settings valid. Therefore, 'reject' must be set to false, and then
    // settings with only 'comment' set are not valid.
    assert.commandFailedWithCode(
        db.adminCommand(
            {setQuerySettings: queryShapeHash, settings: {reject: false, comment: "hi!"}}),
        7746604);
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: query, settings: {reject: false, comment: "hi!"}}),
        7746604);
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration({reject: true}, query)]);

    // Clean-up after the end of the test.
    qsutils.removeAllQuerySettings();
}

{
    // Ensure that 'setQuerySettings' command works for different types of the comment.
    function testCommentField(comment) {
        const query = qsutils.makeFindQueryInstance({filter: {a: 15}});
        const querySettings = {
            indexHints: {ns: {db: db.getName(), coll: collName}, allowedIndexes: [{a: 1}]},
            reject: true,
            comment: comment
        };
        assert.commandWorked(db.adminCommand({
            setQuerySettings: query,
            settings: querySettings,
        }));
        qsutils.assertQueryShapeConfiguration(
            [qsutils.makeQueryShapeConfiguration(querySettings, query)]);
    }

    testCommentField("Modified by: Vlad");
    testCommentField({message: "Hello!"});
    testCommentField(true);
    testCommentField(-1);
    testCommentField(ISODate("2024-01-20T00:00:00Z"));
    testCommentField(null);
    testCommentField(undefined);

    // Clean-up after the end of the test.
    qsutils.removeAllQuerySettings();
}

{
    // Ensure that 'setQuerySettings' command works when some but not all of 'allowedIndex' lists
    // are empty. The second document is ignored in this case.
    const query = qsutils.makeFindQueryInstance({filter: {a: 15}});
    assert.commandWorked(db.adminCommand({
        setQuerySettings: query,
        settings: {
            "indexHints": [
                {"ns": {"db": db.getName(), "coll": "collNameA"}, "allowedIndexes": [{"a": 1}]},
                {"ns": {"db": db.getName(), "coll": "collNameB"}, "allowedIndexes": []},
            ]
        }
    }));

    // Assert that the settings only contain hints for collection with non-empty 'allowedIndexes'.
    qsutils.assertQueryShapeConfiguration([qsutils.makeQueryShapeConfiguration({
        "indexHints":
            [{"ns": {"db": db.getName(), "coll": "collNameA"}, "allowedIndexes": [{"a": 1}]}]
    },
                                                                               query)]);

    // Clean-up after the end of the test.
    qsutils.removeAllQuerySettings();
}

{
    // Start with an empty query settings.
    qsutils.removeAllQuerySettings();
    qsutils.assertQueryShapeConfiguration([]);

    // Ensure that setQuerySettings command fails for query settings with emply 'allowedIndexes'.
    // This is equivalent to query settings without 'indexHints' provided.
    assert.commandFailedWithCode(db.adminCommand({
        setQuerySettings: {find: collName, filter: {a: 123}, $db: db.getName()},
        settings: {
            indexHints: [
                {ns: {db: db.getName(), coll: "collNameA"}, allowedIndexes: []},
                {ns: {db: db.getName(), coll: "collNameB"}, allowedIndexes: []},
            ]
        }
    }),
                                 7746604);

    // Version where 'indexHints' is a single document instead of a list.
    assert.commandFailedWithCode(db.adminCommand({
        setQuerySettings: {find: collName, filter: {a: 123}, $db: db.getName()},
        settings: {
            indexHints: {ns: {db: db.getName(), coll: collName}, allowedIndexes: []}

        }
    }),
                                 7746604);

    // Clean-up after the end of the test.
    qsutils.removeAllQuerySettings();
}

// Test that 'setQuerySettings' reply contains simplified settings.
{
    const query = qsutils.makeFindQueryInstance({filter: {a: 15}});
    const querySettings = {
        indexHints: [{ns: {db: db.getName(), coll: collName}, allowedIndexes: [{a: 1}]}],
        queryFramework: "sbe",
        reject: false
    };
    const simplifiedQuerySettings = {
        indexHints: [{ns: {db: db.getName(), coll: collName}, allowedIndexes: [{a: 1}]}],
        queryFramework: "sbe",
    };

    const reply =
        assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: querySettings}));

    // Verify that the set query settings are simplified.
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(simplifiedQuerySettings, query)]);

    // Verify that reply of "setQuerySettings" command contains simplified query settings.
    assert.eq(reply.settings,
              simplifiedQuerySettings,
              "Reply: " + tojson(reply) + " does not contain simplified query settings: " +
                  tojson(simplifiedQuerySettings));

    qsutils.removeAllQuerySettings();
}
