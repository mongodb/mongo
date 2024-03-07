// Tests setting query settings `reject` flag fails the relevant query (and not others).
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   simulate_atlas_proxy_incompatible,
//   requires_fcv_80,
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

// Creating the collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());

/**
 * Tests that setting `reject` fails the expected query `query`, and a query with the same shape,
 * `queryPrime`, and does _not_ fail a query of differing shape, `unrelatedQuery`.
 */
function testRejection({query, queryPrime, unrelatedQuery}) {
    // Confirm there's no pre-existing settings.
    qsutils.assertQueryShapeConfiguration([]);

    const type = Object.keys(query)[0];
    const getRejectCount = () => db.runCommand({serverStatus: 1}).metrics.commands[type].rejected;

    const rejectBaseline = getRejectCount();

    const assertRejectedDelta = (delta) =>
        assert.soon(() => getRejectCount() == delta + rejectBaseline);

    const getFailedCount = () => db.runCommand({serverStatus: 1}).metrics.commands[type].failed;

    query = qsutils.withoutDollarDB(query);
    queryPrime = qsutils.withoutDollarDB(queryPrime);
    unrelatedQuery = qsutils.withoutDollarDB(unrelatedQuery);

    for (const q of [query, queryPrime, unrelatedQuery]) {
        // With no settings, all queries should succeed.
        assert.commandWorked(db.runCommand(q));
        // And so should explaining those queries.
        assert.commandWorked(db.runCommand({explain: q}));
    }

    // Still nothing has been rejected.
    assertRejectedDelta(0);

    // Set reject flag for query under test.
    assert.commandWorked(db.adminCommand(
        {setQuerySettings: {...query, $db: qsutils.db.getName()}, settings: {reject: true}}));

    // Confirm settings updated.
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration({reject: true},
                                             {...query, $db: qsutils.db.getName()})],
        /* shouldRunExplain */ true);

    // Just setting the reject flag should not alter the rejected cmd counter.
    assertRejectedDelta(0);

    // Verify other query with same shape has those settings applied too.
    qsutils.assertExplainQuerySettings({...queryPrime, $db: qsutils.db.getName()}, {reject: true});

    // Explain should not alter the rejected cmd counter.
    assertRejectedDelta(0);

    const failedBaseline = getFailedCount();
    // The queries with the same shape should both _fail_.
    assert.commandFailedWithCode(db.runCommand(query), ErrorCodes.QueryRejectedBySettings);
    assertRejectedDelta(1);
    assert.commandFailedWithCode(db.runCommand(queryPrime), ErrorCodes.QueryRejectedBySettings);
    assertRejectedDelta(2);

    // Despite some rejections occurring, there should not have been any failures.
    assert.eq(failedBaseline, getFailedCount());

    // Unrelated query should succeed.
    assert.commandWorked(db.runCommand(unrelatedQuery));

    for (const q of [query, queryPrime, unrelatedQuery]) {
        // All explains should still succeed.
        assert.commandWorked(db.runCommand({explain: q}));
    }

    // Explains still should not alter the cmd rejected counter.
    assertRejectedDelta(2);

    // Remove the setting.
    qsutils.removeAllQuerySettings();
    qsutils.assertQueryShapeConfiguration([]);

    // Once again, all queries should succeed.
    for (const q of [query, queryPrime, unrelatedQuery]) {
        assert.commandWorked(db.runCommand(q));
        assert.commandWorked(db.runCommand({explain: q}));
    }

    // Successful, non-rejected queries should not alter the rejected cmd counter.
    assertRejectedDelta(2);
}

testRejection({
    query: qsutils.makeFindQueryInstance({filter: {a: 1}}),
    queryPrime: qsutils.makeFindQueryInstance({filter: {a: 123456}}),
    unrelatedQuery: qsutils.makeFindQueryInstance({filter: {a: "string"}}),
});

testRejection({
    query: qsutils.makeDistinctQueryInstance({key: "k", query: {a: 1}}),
    queryPrime: qsutils.makeDistinctQueryInstance({key: "k", query: {a: 123456}}),
    unrelatedQuery: qsutils.makeDistinctQueryInstance({key: "k", query: {a: "string"}}),
});

let buildPipeline = (matchValue) => [{$match: {matchKey: matchValue}},
                                     {
                                         $group: {
                                             _id: "groupID",
                                             values: {$addToSet: "$value"},
                                         },
                                     },
];

testRejection({
    query: qsutils.makeAggregateQueryInstance({pipeline: buildPipeline(1), cursor: {}}),
    queryPrime: qsutils.makeAggregateQueryInstance({pipeline: buildPipeline(12345), cursor: {}}),
    unrelatedQuery:
        qsutils.makeAggregateQueryInstance({pipeline: buildPipeline("string"), cursor: {}}),
});
