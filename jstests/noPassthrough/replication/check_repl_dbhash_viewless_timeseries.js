/**
 * Regression test for SERVER-133431.
 *
 * Verifies that CheckReplDBHash retries a transient code-491 failure on a legacy buckets namespace.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("CheckReplDBHash legacy timeseries buckets retry", function () {
    let rst;

    before(function () {
        rst = new ReplSetTest({name: jsTestName(), nodes: 2});
        rst.startSet();
        rst.initiate();
    });

    after(function () {
        rst.stopSet();
    });

    it("retries a transient code-491 failure", function () {
        const originalCheckReplicaSet = rst.checkReplicaSet;
        let attempts = 0;

        try {
            // Simulate the error surfaced by DataConsistencyChecker from the first collStats
            // attempt. Subsequent calls succeed, proving that CheckReplDBHash retries this
            // specific failure.
            rst.checkReplicaSet = () => {
                if (attempts++ === 0) {
                    const error = new Error("collStats failed with code 491");
                    error.code = ErrorCodes.CommandNotSupportedOnLegacyTimeseriesBucketsNamespace;
                    throw error;
                }
            };
            assert.doesNotThrow(() => rst.checkReplicatedDataHashes());
            assert.eq(2, attempts, "CheckReplDBHash did not retry code 491");
        } finally {
            rst.checkReplicaSet = originalCheckReplicaSet;
        }
    });

    it("does not retry when another collStats error is present", function () {
        const originalCheckReplicaSet = rst.checkReplicaSet;
        let attempts = 0;

        try {
            // If one node reports code 491 but the other reports a different collStats error,
            // DataConsistencyChecker reports a regular dbhash mismatch. CheckReplDBHash must
            // propagate that failure without retrying it.
            rst.checkReplicaSet = () => {
                attempts++;
                const error = new Error(
                    "dbhash mismatch: one node returned code 491 and the other returned a " +
                        "different error",
                );
                error.code = ErrorCodes.OperationFailed;
                throw error;
            };
            assert.throws(() => rst.checkReplicatedDataHashes());
            assert.eq(1, attempts, "CheckReplDBHash incorrectly retried a non-491 failure");
        } finally {
            rst.checkReplicaSet = originalCheckReplicaSet;
        }
    });
});
