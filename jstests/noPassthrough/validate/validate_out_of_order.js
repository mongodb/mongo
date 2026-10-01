/**
 * Tests that out-of-order keys are detected by validation during both the collection and index scan
 * phases.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let coll = primary.getCollection("test.out_of_order");
assert.commandWorked(coll.createIndex({x: 1}));

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

// Test record store out-of-order detection.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "failRecordStoreTraversal", mode: "alwaysOn"}),
);
let res = assert.commandWorked(coll.validate());
assert(!res.valid);
// The out-of-order record store error must be disambiguated with the log id (12890000) of the
// entry that carries the offending record details.
assert(
    res.errors.some((e) => e.includes("out-of-order") && e.includes("12890000")),
    "Expected disambiguated out-of-order error referencing log id 12890000",
    {errors: res.errors},
);
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "failRecordStoreTraversal", mode: "off"}),
);

// Test index entry out-of-order detection.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "failIndexKeyOrdering", mode: "alwaysOn"}),
);
res = assert.commandWorked(coll.validate());
assert(!res.valid);
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "failIndexKeyOrdering", mode: "off"}),
);

rst.stopSet();
