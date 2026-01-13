/**
 * Tests that `capMemoryConsumptionForPreAuthBuffers` parameter validation fails at startup
 * when set to invalid values (0 and 101), rather than at runtime.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

(function testCapMemoryConsumptionPreAuthBuffersValidation() {
    jsTest.log("Testing parameter validation for capMemoryConsumptionForPreAuthBuffers");

    // Test 1: Value 0 should fail (must be > 0)
    jsTest.log("Testing that value 0 fails at startup");
    assert.throws(
        () => MongoRunner.runMongod({
            setParameter: {
                capMemoryConsumptionForPreAuthBuffers: 0,
            },
        }),
        [],
        "Expected mongod to fail to startup with capMemoryConsumptionForPreAuthBuffers=0");

    // Test 2: Value 101 should fail (must be <= 100)
    jsTest.log("Testing that value 101 fails at startup");
    assert.throws(
        () => MongoRunner.runMongod({
            setParameter: {
                capMemoryConsumptionForPreAuthBuffers: 101,
            },
        }),
        [],
        "Expected mongod to fail to startup with capMemoryConsumptionForPreAuthBuffers=101");

    // Test 3: Valid value 1 should succeed
    jsTest.log("Testing that valid value 1 succeeds at startup");
    let conn = MongoRunner.runMongod({
        setParameter: {
            capMemoryConsumptionForPreAuthBuffers: 1,
        },
    });
    assert.neq(
        null,
        conn,
        "Expected mongod to start successfully with capMemoryConsumptionForPreAuthBuffers=1");
    MongoRunner.stopMongod(conn);

    jsTest.log("All parameter validation tests passed");
})();
