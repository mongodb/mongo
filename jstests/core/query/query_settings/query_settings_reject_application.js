// Tests setting query settings `reject` flag fails the relevant query (and not others).
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   simulate_atlas_proxy_incompatible,
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

    query = qsutils.withoutDollarDB(query);
    queryPrime = qsutils.withoutDollarDB(queryPrime);
    unrelatedQuery = qsutils.withoutDollarDB(unrelatedQuery);

    for (const q of [query, queryPrime, unrelatedQuery]) {
        // With no settings, all queries should succeed.
        assert.commandWorked(db.runCommand(q));
        // And so should explaining those queries.
        assert.commandWorked(db.runCommand({explain: q}));
    }

    // Set reject flag for query under test.
    assert.commandWorked(db.adminCommand(
        {setQuerySettings: {...query, $db: qsutils.db.getName()}, settings: {reject: true}}));

    // Confirm settings updated.
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration({reject: true},
                                             {...query, $db: qsutils.db.getName()})],
        /* shouldRunExplain */ true);
    // Verify other query with same shape has those settings applied too.
    qsutils.assertExplainQuerySettings({...queryPrime, $db: qsutils.db.getName()}, {reject: true});

    // The queries with the same shape should both _fail_.
    assert.commandFailedWithCode(db.runCommand(query), ErrorCodes.QueryRejectedBySettings);
    assert.commandFailedWithCode(db.runCommand(queryPrime), ErrorCodes.QueryRejectedBySettings);

    // Unrelated query should succeed.
    assert.commandWorked(db.runCommand(unrelatedQuery));

    for (const q of [query, queryPrime, unrelatedQuery]) {
        // All explains should still succeed.
        assert.commandWorked(db.runCommand({explain: q}));
    }

    // Remove the setting.
    qsutils.removeAllQuerySettings();
    qsutils.assertQueryShapeConfiguration([]);

    // Once again, all queries should succeed.
    for (const q of [query, queryPrime, unrelatedQuery]) {
        assert.commandWorked(db.runCommand(q));
        assert.commandWorked(db.runCommand({explain: q}));
    }
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
