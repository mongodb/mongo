/**
 * This test checks the result of validating the fast count collection type with and without
 * enforceFastCount: true.
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const collName = jsTestName() + "_coll";

describe("FastCount validation", function () {
    beforeEach(function () {
        this.rst = new ReplSetTest({nodes: 1});
        this.rst.startSet();
        this.rst.initiate();
        this.conn = this.rst.getPrimary();
        this.db = this.conn.getDB("test");
        this.configDb = this.conn.getDB("config");

        assert.commandWorked(this.db.createCollection(collName));
        assert.commandWorked(this.db[collName].insert({x: 1}));
    });

    it("detects correct FastCountType", function () {
        const enforceFlagCombinations = [
            {enforceFastCount: false, enforceFastSize: false},
            {enforceFastCount: true},
            {enforceFastSize: true},
            {enforceFastCount: true, enforceFastSize: true},
        ];
        const featureFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(this.db, "featureFlagReplicatedFastCount");
        const expectedFastCountType = featureFlagEnabled ? "both" : "legacySizeStorer";

        for (const flag of enforceFlagCombinations) {
            const result = assert.commandWorked(this.db.runCommand(Object.assign({validate: collName}, flag)));
            assert(result.valid, result);
            assert.eq(result.fastCountType, expectedFastCountType, result);

            // The keysPerIndex values are populated by _validateIndexes(), which runs after traverseRecordStore(), so a
            // non-zero key count for the _id index confirms the fast count error did not stop validation early. The
            // keysPerIndex field itself is always present (populated by _validateIndexesInternalStructure() before
            // traversal), but the key counts remain 0 unless _validateIndexes() actually runs.
            assert.eq(
                result.keysPerIndex._id_,
                1,
                "Expected _id index to have 1 key traversed, indicating _validateIndexes() ran: " + tojson(result),
            );
        }
    });

    // TOOD(SERVER-115100): Write a test to verify that neither the WT size storer nor the
    // replicated fast count collection is created.
    //
    // Until the WiredTiger size storer table is removed, testing the "neither" case is not easy.
    // Even if you delete the file, the catalog still contains an entry for the table. Thus the
    // behavior is not tested here.

    afterEach(function () {
        this.rst.stopSet();
    });
});
