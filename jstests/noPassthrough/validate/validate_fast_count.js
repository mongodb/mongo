/**
 * This test checks the result of validating the fast count collection type with and without
 * enforceFastCount: true.
 *
 * TODO(SERVER-117326): Remove, which may involve updating the assertions in this test.
 * @tags: [
 *   featureFlagReplicatedFastCount_incompatible,
 * ]
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const internalFastCountCollectionName = "fast_count_metadata_store";

function initializeMongod(ctx) {
    ctx.conn = MongoRunner.runMongod({});
    ctx.db = ctx.conn.getDB("test");
    ctx.configDb = ctx.conn.getDB("config");

    assert.commandWorked(ctx.db.createCollection("testColl"));
    assert.commandWorked(ctx.db.testColl.insert({x: 1}));
}

describe("FastCount validation", function () {
    beforeEach(function () {
        initializeMongod(this);
    });

    it("detects legacy WiredTiger size storer table", function () {
        const resultWithoutEnforce = assert.commandWorked(this.db.runCommand({validate: "testColl"}));
        assert(resultWithoutEnforce.valid, resultWithoutEnforce);
        assert(resultWithoutEnforce.fastCountType == "legacySizeStorer", resultWithoutEnforce);

        const resultWithEnforce = assert.commandWorked(
            this.db.runCommand({validate: "testColl", enforceFastCount: true}),
        );
        assert(resultWithEnforce.valid, resultWithEnforce);
        assert(resultWithEnforce.fastCountType == "legacySizeStorer", resultWithEnforce);
    });

    afterEach(function () {
        MongoRunner.stopMongod(this.conn);
    });
});

describe("FastCount validation without shutdown validation", function () {
    beforeEach(function () {
        initializeMongod(this);
    });

    it("detects both after creating replicated collection", function () {
        assert.commandWorked(this.configDb.createCollection(internalFastCountCollectionName));

        const resultWithoutEnforce = assert.commandWorked(this.db.runCommand({validate: "testColl"}));
        assert(resultWithoutEnforce.valid, resultWithoutEnforce);
        assert(resultWithoutEnforce.fastCountType == "both", resultWithoutEnforce);

        const resultWithEnforce = assert.commandWorked(
            this.db.runCommand({validate: "testColl", enforceFastCount: true}),
        );
        assert(!resultWithEnforce.valid, resultWithEnforce);
        assert(resultWithEnforce.fastCountType == "both", resultWithEnforce);

        // The keysPerIndex values are populated by _validateIndexes(), which runs after
        // traverseRecordStore(), so a non-zero key count for the _id index confirms the fast count
        // error did not stop validation early. The keysPerIndex field itself is always present
        // (populated by _validateIndexesInternalStructure() before traversal), but the key counts
        // remain 0 unless _validateIndexes() actually runs.
        assert.eq(
            resultWithEnforce.keysPerIndex._id_,
            1,
            "Expected _id index to have 1 key traversed, indicating _validateIndexes() ran: " +
                tojson(resultWithEnforce),
        );
    });

    // TOOD(SERVER-115100): Write a test to verify that neither the WT size storer nor the
    // replicated fast count collection is created.
    //
    // Until the WiredTiger size storer table is removed, testing the "neither" case is not easy.
    // Even if you delete the file, the catalog still contains an entry for the table. Thus the
    // behavior is not tested here.

    afterEach(function () {
        // We must skip validation during shutdown because creating the `fast_count_metadata_store`
        // causes the shutdown validation result to be invalid.
        MongoRunner.stopMongod(this.conn, null, {skipValidation: true});
    });
});
