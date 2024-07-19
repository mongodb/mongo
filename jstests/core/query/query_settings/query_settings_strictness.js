/**
 * Tests that the query settings object accepts arbitrary fields to avoid having breaking changes in
 * the future.
 * @tags: [
 *   directly_against_shardsvrs_incompatible,
 *   requires_non_retryable_commands,
 *   simulate_atlas_proxy_incompatible,
 *   requires_fcv_80,
 *   # TODO SERVER-89461 Investigate why test using huge batch size timeout in suites with balancer.
 *   assumes_balancer_off,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());
const query = qsutils.makeFindQueryInstance({filter: {a: 15}});
const validSettings = {
    queryFramework: "classic",
};
qsutils.withQuerySettings(query, {...validSettings, unknownField: "some value"}, () => {
    // Ensure that only the valid fields are present in both the $querySettings and explain output.
    // The 'unknownField' should be ignored.
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(validSettings, query)]);
});
