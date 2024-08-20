/**
 * Define overrides to prevent any test from spawning its own test fixture since certain passthrough
 * suites should not contain JS tests that start their own mongod/s.
 */
MongoRunner.runMongod = function() {
    throw new Error("Detected MongoRunner.runMongod() call in js test from passthrough suite. " +
                    "Consider moving the test to one of the jstests/noPassthrough/, " +
                    "jstests/replsets/, or jstests/sharding/ directories.");
};

MongoRunner.runMongos = function() {
    throw new Error("Detected MongoRunner.runMongos() call in js test from passthrough suite. " +
                    "Consider moving the test to one of the jstests/noPassthrough/, " +
                    "jstests/replsets/, or jstests/sharding/ directories.");
};

import {
    ShardingTest,
    kOverrideConstructor as kOverrideConstructorForST
} from "jstests/libs/shardingtest.js";
import {
    ReplSetTest,
    kOverrideConstructor as kOverrideConstructorForRST
} from "jstests/libs/replsettest.js";

ShardingTest[kOverrideConstructorForST] = class NoSpawnShardingTest extends ShardingTest {
    constructor() {
        throw new Error("Detected ShardingTest() call in js test from passthrough suite. " +
                        "Consider moving the test to one of the jstests/noPassthrough/, " +
                        "jstests/replsets/, or jstests/sharding/ directories.");
    }
};

ReplSetTest[kOverrideConstructorForRST] = class NoSpawnReplSetTest extends ReplSetTest {
    constructor(opts) {
        if (typeof opts !== 'string' && !(opts instanceof String)) {
            throw new Error("Detected ReplSetTest() call in js test from passthrough suite. " +
                            "Consider moving the test to one of the jstests/noPassthrough/, " +
                            "jstests/replsets/, or jstests/sharding/ directories.");
        }

        super(opts);
    }
};
