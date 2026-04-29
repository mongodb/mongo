/**
 * Tests that the validate user repair command removes corrupt documents and fixes index
 * inconsistencies.
 */

import {beforeEach, describe, it} from "jstests/libs/mochalite.js";

describe("It detects corrupt documents in validation", () => {
    let coll;
    beforeEach(() => {
        coll = db.getCollection(jsTestName());
        coll.drop();

        // Corrupt document during insert for testing via failpoint.
        assert.commandWorked(
            db.adminCommand({
                configureFailPoint: "corruptDocumentOnInsert",
                data: {"ns": coll.getFullName()},
                mode: "alwaysOn",
            }),
        );
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorked(db.adminCommand({configureFailPoint: "corruptDocumentOnInsert", mode: "off"}));
    });

    it("Ensures validate detects the corrupt document", () => {
        // Ensure validate detects corrupt document.
        const output = coll.validate({full: true});
        assert.eq(output.valid, false, "validate returned valid true when expected false: " + tojson(output));
        assert.eq(output.repaired, false, "validate returned repaired true when expected false: " + tojson(output));
        assert.eq(
            output.nInvalidDocuments,
            1,
            "validate returned an invalid number of invalid documents: " + tojson(output),
        );
        assert.eq(output.nrecords, 1, "validate returned an invalid number of records: " + tojson(output));
        assert.eq(
            output.corruptRecords,
            [NumberLong(1)],
            "validate returned an invalid corruptRecords: " + tojson(output),
        );

        // The corrupt record should surface the BSON validation failure both in the validate
        // results' `errors` array and via log id 12395400.
        assert(
            output.errors.some((e) => e.includes("12395400")),
            "expected validate errors to reference log id 12395400: " + tojson(output),
        );
        assert.soon(
            () => checkLog.checkContainsOnceJson(db.getMongo(), 12395400),
            "expected log id 12395400 ('Error occurred during BSON validation') to be emitted",
        );
    });

    it(
        "Ensures validate with repair mode removes the corrupt document." + //
            "Removing corrupt document results in extra entry in index _id. " + //
            "Repair mode should also remove the extra index entry.",
        () => {
            // Ensure validate with repair mode removes the corrupt document. Removing corrupt document results
            // in extra entry in index _id. Repair mode should also remove the extra index entry.
            const output = coll.validate({full: true, repair: true});
            assert.eq(output.valid, true, "validate returned valid false when expected true" + tojson(output));
            assert.eq(output.repaired, true, "validate returned repaired false when expected true" + tojson(output));
            assert.eq(
                output.nInvalidDocuments,
                0,
                "validate returned an invalid number of invalid documents" + tojson(output),
            );
            assert.eq(output.nrecords, 0, "validate returned an invalid number of records" + tojson(output));
            assert.eq(output.corruptRecords, [], "validate returned an invalid corruptRecords: " + tojson(output));
            assert.eq(
                output.numRemovedCorruptRecords,
                1,
                "validate returned an invalid number of removed corrupt records" + tojson(output),
            );
            assert.eq(
                output.numRemovedExtraIndexEntries,
                1,
                "validate returned an invalid number of removed extra index entries" + tojson(output),
            );
            assert.eq(output.keysPerIndex._id_, 0, "expected 0 keys in index _id: " + tojson(output));
            assert.eq(
                output.indexDetails._id_.valid,
                true,
                "validate returned indexDetails valid false when expected true" + tojson(output),
            );

            // Confirm validate results are valid and repair mode did not silently suppress validation errors.
            const revalidateOutput = coll.validate({full: true});
            assert.eq(revalidateOutput.valid, true, "validate returned valid false when expected true");
            assert.eq(revalidateOutput.repaired, false, "validate returned repaired true when expected false");
        },
    );
});
