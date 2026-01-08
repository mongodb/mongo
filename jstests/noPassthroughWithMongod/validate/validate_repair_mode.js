/**
 * Tests that the validate user repair command removes corrupt documents and fixes index
 * inconsistencies.
 */

const coll = db.getCollection(jsTestName());
coll.drop();

// Corrupt document during insert for testing via failpoint.
const numCorruptDocs = 2000;
assert.commandWorked(
    db.adminCommand({
        configureFailPoint: "corruptDocumentOnInsert",
        data: {"collectionName": coll.getName()},
        mode: "alwaysOn",
    }),
);
for (let i = 0; i < numCorruptDocs; ++i) {
    assert.commandWorked(coll.insert({a: String(i)}));
}
assert.commandWorked(db.adminCommand({configureFailPoint: "corruptDocumentOnInsert", mode: "off"}));

// Ensure validate detects corrupt document.
let output = coll.validate({full: true});
assert.eq(output.valid, false, "validate returned valid true when expected false: " + tojson(output));
assert.eq(output.repaired, false, "validate returned repaired true when expected false: " + tojson(output));
assert.eq(
    output.nInvalidDocuments,
    numCorruptDocs,
    "validate returned an invalid number of invalid documents: " + tojson(output),
);
assert.eq(output.nrecords, numCorruptDocs, "validate returned an invalid number of records: " + tojson(output));

const corruptRecords = Array.from({length: numCorruptDocs}, (_, index) => NumberLong(index + 1));

assert.eq(output.corruptRecords, corruptRecords, "validate returned an invalid corruptRecords: " + tojson(output));

// Ensure validate with repair mode removes the corrupt document. Removing corrupt document results
// in extra entry in index _id. Repair mode should also remove the extra index entry.
output = coll.validate({full: true, repair: true});
assert.eq(output.valid, true, "validate returned valid false when expected true" + tojson(output));
assert.eq(output.repaired, true, "validate returned repaired false when expected true" + tojson(output));
assert.eq(output.nInvalidDocuments, 0, "validate returned an invalid number of invalid documents" + tojson(output));
assert.eq(output.nrecords, 0, "validate returned an invalid number of records" + tojson(output));
assert.eq(output.corruptRecords, [], "validate returned an invalid corruptRecords: " + tojson(output));
assert.eq(
    output.numRemovedCorruptRecords,
    numCorruptDocs,
    "validate returned an invalid number of removed corrupt records" + tojson(output),
);
assert.eq(
    output.numRemovedExtraIndexEntries,
    numCorruptDocs,
    "validate returned an invalid number of removed extra index entries" + tojson(output),
);
assert.eq(output.keysPerIndex._id_, 0, "expected 0 keys in index _id: " + tojson(output));
assert.eq(
    output.indexDetails._id_.valid,
    true,
    "validate returned indexDetails valid false when expected true" + tojson(output),
);

// Confirm validate results are valid and repair mode did not silently suppress validation errors.
output = coll.validate({full: true});
assert.eq(output.valid, true, "validate returned valid false when expected true");
assert.eq(output.repaired, false, "validate returned repaired true when expected false");
