/**
 * Test configuring feature flags at startup, using --setParameter, and at runtime.
 */

import {getParameter} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const standalone = MongoRunner.runMongod(
    {setParameter: {featureFlagFork: false, featureFlagInDevelopmentForTest: true}});
try {
    // By default, featureFlagFork is _enabled_ and featureFlagInDevelopmentForTest is _disabled_,
    // but they should take on the values from '--setParameter' (as passed by 'runMongod').
    assert.eq(getParameter(standalone, "featureFlagFork").value, false);
    assert.eq(getParameter(standalone, "featureFlagInDevelopmentForTest").value, true);

    // Normal feature flags should not be settable at runtime.
    assert.commandFailedWithCode(standalone.adminCommand({setParameter: 1, featureFlagFork: false}),
                                 ErrorCodes.IllegalOperation);

    // Incremental Feature Rollout (IFR) flags _should_ be settable at runtime.
    assert.commandWorked(
        standalone.adminCommand({setParameter: 1, featureFlagInDevelopmentForTest: false}));
    assert.eq(getParameter(standalone, "featureFlagInDevelopmentForTest").value, false);
} finally {
    MongoRunner.stopMongod(standalone);
}
