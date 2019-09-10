// Tests the _runningMongoChildProcessIds shell built-in.

(function() {
'use strict';

/**
 * @param {NumberLong[]} expected pids
 */
function assertRunningMongoChildProcessIds(expected) {
    const expectedSorted = expected.sort();
    const children = MongoRunner.runningChildPids().sort();
    assert.eq(expectedSorted, children);
}

// Empty before we start anything.
assertRunningMongoChildProcessIds([]);

(function() {
// Start 1 mongod.
const mongod = MongoRunner.runMongod({});
try {
    // Call 3 times just for good-measure.
    assertRunningMongoChildProcessIds([mongod.pid]);
    assertRunningMongoChildProcessIds([mongod.pid]);
    assertRunningMongoChildProcessIds([mongod.pid]);
} finally {
    MongoRunner.stopMongod(mongod);
}
})();

(function() {
// Start 2 mongods.
const mongod1 = MongoRunner.runMongod({});
const mongod2 = MongoRunner.runMongod({});
try {
    assertRunningMongoChildProcessIds([mongod1.pid, mongod2.pid]);

    // Stop mongod1 and only mongod2 should remain.
    MongoRunner.stopMongod(mongod1);
    assertRunningMongoChildProcessIds([mongod2.pid]);
} finally {
    // It is safe to stop multiple times.
    MongoRunner.stopMongos(mongod2);
    MongoRunner.stopMongos(mongod1);
}
})();

// empty at the end
assertRunningMongoChildProcessIds([]);
})();
