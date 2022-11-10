// SERVER-28594 Ensure non-atomic ops are individually logged in applyOps
// and atomic ops are collectively logged in applyOps.
// @tags: [requires_replication]
(function() {
"use strict";

let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testDB = primary.getDB("test");
let oplogColl = primary.getDB("local").oplog.rs;
let testCollName = "testColl";
let rerenamedCollName = "rerenamedColl";

testDB.runCommand({drop: testCollName});
testDB.runCommand({drop: rerenamedCollName});
assert.commandWorked(testDB.runCommand({create: testCollName}));
let testColl = testDB[testCollName];

// Ensure applyOps logging produces an oplog entry for each operation in the applyOps call and no
// record of applyOps appears for these operations.
assert.commandWorked(testDB.runCommand({
    applyOps: [
        {
            op: "c",
            ns: "test.$cmd",
            o: {
                renameCollection: "test.testColl",
                to: "test.renamedColl",
                stayTemp: false,
                dropTarget: false
            }
        },
        {
            op: "c",
            ns: "test.$cmd",
            o: {
                renameCollection: "test.renamedColl",
                to: "test." + rerenamedCollName,
                stayTemp: false,
                dropTarget: false
            }
        }
    ]
}));
assert.eq(oplogColl.find({"o.renameCollection": {"$exists": true}}).count(), 2);
assert.eq(oplogColl.find({"o.applyOps": {"$exists": true}}).count(), 0);

// Since atomic applyOps cannot run atomically, this test ensures that applyOps ignores the
// 'allowAtomic' field. We fail to insert a duplicate doc, and ensure that the original doc was
// inserted successfully. Atomic applyOps would fail to insert both documents.
assert.commandWorked(testDB.createCollection(testColl.getName()));
assert.commandWorked(testDB.runCommand({
    applyOps: [
        {op: "i", ns: testColl.getFullName(), o: {_id: 3, a: "augh"}},
        {op: "i", ns: testColl.getFullName(), o: {_id: 4, a: "blah"}}
    ],
    allowAtomic: true,
}));

assert.eq(oplogColl.find({"o.applyOps": {"$exists": true}}).count(), 0);
assert.eq(oplogColl.find({op: "i", ns: testColl.getFullName()}).count(), 2);

rst.stopSet();
})();
