/**
 * Define overrides to be loaded in the core.yml suite.
 * Since the core.yml suite should not contain JS tests that start their own mongod/s, these
 * overrides are to catch any JS tests that tries to spawn their own mongod by throwing an error.
 */
(function() {
    'use strict';

    MongoRunner.runMongod = function() {
        throw new Error("Detected MongoRunner.runMongod() call in js test from core.yml suite. " +
                        "Consider moving the test to one of the jstests/noPassthrough/, " +
                        "jstests/replsets/, or jstests/sharding/ directories.");
    };

    MongoRunner.runMongos = function() {
        throw new Error("Detected MongoRunner.runMongos() call in js test from core.yml suite. " +
                        "Consider moving the test to one of the jstests/noPassthrough/, " +
                        "jstests/replsets/, or jstests/sharding/ directories.");
    };

    const STOverrideConstructor = function() {
        throw new Error("Detected ShardingTest() call in js test from core.yml suite. " +
                        "Consider moving the test to one of the jstests/noPassthrough/, " +
                        "jstests/replsets/, or jstests/sharding/ directories.");
    };

    // This Object.assign() lets us modify ShardingTest to use the new overridden constructor but
    // still keep any static properties it has.
    ShardingTest = Object.assign(STOverrideConstructor, ShardingTest);

    const RSTOverrideConstructor = function() {
        throw new Error("Detected ReplSetTest() call in js test from core.yml suite. " +
                        "Consider moving the test to one of the jstests/noPassthrough/, " +
                        "jstests/replsets/, or jstests/sharding/ directories.");
    };

    // Same as the above Object.assign() call. In particular, we want to preserve the
    // ReplSetTest.kDefaultTimeoutMS property, which should be accessible to tests in the
    // passthrough suite.
    ReplSetTest = Object.assign(RSTOverrideConstructor, ReplSetTest);
})();
