/**
 * Tests that the query stats store can be resized.
 */

function testQueryStatsSetting(paramName, paramValue) {
    // The feature flag is enabled - make sure the queryStats store can be configured.
    const original = assert.commandWorked(db.adminCommand({getParameter: 1, [paramName]: 1}));
    assert(original.hasOwnProperty(paramName), original);
    const originalValue = original[paramName];
    try {
        assert.doesNotThrow(() => db.adminCommand({setParameter: 1, [paramName]: paramValue}));
        // Other tests verify that changing the parameter actually affects the behavior.
    } finally {
        assert.doesNotThrow(() => db.adminCommand({setParameter: 1, [paramName]: originalValue}));
    }
}

testQueryStatsSetting("internalQueryStatsCacheSize", "2MB");
testQueryStatsSetting("internalQueryStatsRateLimit", 2147483647);
