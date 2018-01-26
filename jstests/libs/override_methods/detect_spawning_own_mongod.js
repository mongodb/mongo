/**
 * Define overrides to prevent any test from spawning its own test fixture since certain passthrough
 * suites should not contain JS tests that start their own mongod/s.
 */
(function() {
    'use strict';

    MongoRunner.runMongod = function() {
        throw new Error(
            "Detected MongoRunner.runMongod() call in js test from passthrough suite. " +
            "Consider moving the test to one of the jstests/noPassthrough/, " +
            "jstests/replsets/, or jstests/sharding/ directories.");
    };

    MongoRunner.runMongos = function() {
        throw new Error(
            "Detected MongoRunner.runMongos() call in js test from passthrough suite. " +
            "Consider moving the test to one of the jstests/noPassthrough/, " +
            "jstests/replsets/, or jstests/sharding/ directories.");
    };

    ShardingTest = function() {
        throw new Error("Detected ShardingTest() call in js test from passthrough suite. " +
                        "Consider moving the test to one of the jstests/noPassthrough/, " +
                        "jstests/replsets/, or jstests/sharding/ directories.");
    };

    ReplSetTest = function() {
        throw new Error("Detected ReplSetTest() call in js test from passthrough suite. " +
                        "Consider moving the test to one of the jstests/noPassthrough/, " +
                        "jstests/replsets/, or jstests/sharding/ directories.");
    };

})();
