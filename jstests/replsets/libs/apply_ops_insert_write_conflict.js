/**
 * Sets up a test for WriteConflictException handling in applyOps with an insert workload.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

export var ApplyOpsInsertWriteConflictTest = function (options) {
    // Skip db hash check because this test may throw write conflicts and collmod fails.
    TestData.skipCheckDBHashes = true;

    if (!(this instanceof ApplyOpsInsertWriteConflictTest)) {
        return new ApplyOpsInsertWriteConflictTest(options);
    }

    // Capture the 'this' reference
    let self = this;

    self.options = options;

    /**
     * Runs the test.
     */
    this.run = function () {
        let options = this.options;

        let replTest = new ReplSetTest({nodes: 1});
        replTest.startSet();
        replTest.initiate();

        let primary = replTest.getPrimary();
        let primaryDB = primary.getDB("test");

        let t = primaryDB.getCollection(options.testName);
        t.drop();

        assert.commandWorked(primaryDB.createCollection(t.getName()));

        let numOps = 1000;
        let ops = Array(numOps)
            .fill("ignored")
            .map((unused, i) => {
                return {op: "i", ns: t.getFullName(), o: {_id: i}};
            });

        // Probabilities for WCE are chosen based on empirical testing.
        let probability = 5.0 / numOps;

        // Set up failpoint to trigger WriteConflictException during write operations.
        assert.commandWorked(primaryDB.adminCommand({setParameter: 1, traceWriteConflictExceptions: true}));
        assert.commandWorked(
            primaryDB.adminCommand({
                configureFailPoint: "WTWriteConflictException",
                mode: {activationProbability: probability},
            }),
        );

        // This logs each operation being applied.
        let previousLogLevel = assert.commandWorked(primaryDB.setLogLevel(3, "replication")).was.replication.verbosity;

        let applyOpsResult = primaryDB.adminCommand({applyOps: ops});

        // Reset log level.
        primaryDB.setLogLevel(previousLogLevel, "replication");

        assert.eq(
            numOps,
            applyOpsResult.applied,
            "number of operations applied did not match list of generated insert operations. " +
                "applyOps result: " +
                tojson(applyOpsResult),
        );
        applyOpsResult.results.forEach((operationSucceeded, i) => {
            assert(
                operationSucceeded,
                "applyOps failed: operation with index " +
                    i +
                    " failed: operation: " +
                    tojson(ops[i], "", true) +
                    ". applyOps result: " +
                    tojson(applyOpsResult),
            );
        });
        assert.commandWorked(applyOpsResult);

        replTest.stopSet();
    };
};
