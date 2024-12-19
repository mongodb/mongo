// Tests setting query settings `reject` flag fails the relevant query (and not others).
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   requires_fcv_80,
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

// Creating the collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());
qsutils.removeAllQuerySettings();

qsutils.assertRejection({
    query: qsutils.makeFindQueryInstance({filter: {a: 1}}),
    queryPrime: qsutils.makeFindQueryInstance({filter: {a: 123456}}),
    unrelatedQuery: qsutils.makeFindQueryInstance({filter: {a: "string"}}),
});

qsutils.assertRejection({
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

qsutils.assertRejection({
    query: qsutils.makeAggregateQueryInstance({pipeline: buildPipeline(1), cursor: {}}),
    queryPrime: qsutils.makeAggregateQueryInstance({pipeline: buildPipeline(12345), cursor: {}}),
    unrelatedQuery:
        qsutils.makeAggregateQueryInstance({pipeline: buildPipeline("string"), cursor: {}}),
});
