/*
 * Test that verifies getDiagnosticData returns FTDC data using the new format.
 *
 * @tags: [
 *  requires_fcv_80,
 *  # TODO (SERVER-87187): the mongo shell crashes with stepdowns. Investigate why.
 *  does_not_support_stepdowns,
 * ]
 */

import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

const setFeatureFlag = {
    setParameter: {featureFlagRouterPort: true}
};
var st = new ShardingTest({
    embeddedRouter: true,
    configShard: true,
    shards: 1,
    rs: setFeatureFlag,
    mongos: setFeatureFlag,
});

const db = st.rs0.getPrimary().getDB("admin");
const data = verifyGetDiagnosticData(db.getSiblingDB("admin"), true, true);

// Check that the new format sections are there.
assert(data.hasOwnProperty("shard"));
assert(data.hasOwnProperty("router"));
assert(data.hasOwnProperty("common"));
assert(data.shard.hasOwnProperty("start"));
assert(data.shard.hasOwnProperty("end"));
assert(data.router.hasOwnProperty("start"));
assert(data.router.hasOwnProperty("end"));
assert(data.common.hasOwnProperty("start"));
assert(data.common.hasOwnProperty("end"));

st.stop();
