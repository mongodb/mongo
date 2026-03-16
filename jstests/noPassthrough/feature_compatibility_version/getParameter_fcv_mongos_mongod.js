/**
 * Tests the behavior of `getParameter` for `featureCompatibilityVersion` on mongod vs mongos.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, mongos: 1});

// On mongos, the `featureCompatibilityVersion` server parameter is NOT registered.
{
    const db = st.s.getDB("admin");

    assert.commandFailedWithCode(
        db.runCommand({getParameter: 1, featureCompatibilityVersion: 1}),
        ErrorCodes.InvalidOptions,
    );

    const allParamsRes = assert.commandWorked(db.runCommand({getParameter: "*"}));
    assert(!allParamsRes.hasOwnProperty("featureCompatibilityVersion"));
}

// On mongod, the `featureCompatibilityVersion` is available and returns the current FCV state.
{
    const db = st.rs0.getPrimary().getDB("admin");

    const res = assert.commandWorked(db.runCommand({getParameter: 1, featureCompatibilityVersion: 1}));

    assert(res.hasOwnProperty("featureCompatibilityVersion"));
    assert(res.featureCompatibilityVersion.hasOwnProperty("version"));
    assert.eq(res.featureCompatibilityVersion.version, latestFCV);

    const allParamsRes = assert.commandWorked(db.runCommand({getParameter: "*"}));
    assert(allParamsRes.hasOwnProperty("featureCompatibilityVersion"));
}

st.stop();
