/**
 * Test that densify fails if the partition table exceeds the memory limit.
 * @tags: [
 *   # Needed as $densify is a 51 feature.
 *   requires_fcv_51,
 *   not_allowed_with_signed_security_token,
 *   # This test sets a server parameter via setParameterOnAllNonConfigNodes. To keep the host list
 *   # consistent, no add/remove shard operations should occur during the test.
 *   assumes_stable_shard_list,
 * ]
 */

import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const origParamValue = assert.commandWorked(db.adminCommand({
    getParameter: 1,
    internalDocumentSourceDensifyMaxMemoryBytes: 1
}))["internalDocumentSourceDensifyMaxMemoryBytes"];

// Lower limit for testing.
setParameterOnAllNonConfigNodes(db.getMongo(), "internalDocumentSourceDensifyMaxMemoryBytes", 1000);

const coll = db[jsTestName()];
coll.drop();
let numDocs = 10;
// Create a string entirely of comma characters.
let longString = Array(101).toString();
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({_id: i, val: i, part: longString + i});
}
bulk.execute();

const pipeline = {
    $densify: {field: "val", partitionByFields: ["part"], range: {step: 1, bounds: "partition"}}
};

assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: [pipeline], cursor: {}}), 6007200);

// Test that densify succeeds when the memory limit would be exceeded, but documents don't need to
// be densified.
coll.drop();
bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({_id: i, otherVal: i, part: longString + i});
}
bulk.execute();
assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: [pipeline], cursor: {}}));

// Reset limit for other tests.
setParameterOnAllNonConfigNodes(db.getMongo(), "internalDocumentSourceDensifyMaxMemoryBytes", origParamValue);
