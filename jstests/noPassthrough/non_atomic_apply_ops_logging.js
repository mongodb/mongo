// SERVER-28594 Ensure non-atomic ops are individually logged in applyOps
// and atomic ops are collectively logged in applyOps.
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

    // Ensure atomic apply ops logging only produces one oplog entry
    // per call to apply ops and does not log individual operations
    // separately.
    assert.commandWorked(testDB.runCommand({
        applyOps: [
            {op: "i", ns: testColl.getFullName(), o: {_id: 1, a: "foo"}},
            {op: "i", ns: testColl.getFullName(), o: {_id: 2, a: "bar"}}
        ]
    }));
    assert.eq(oplogColl.find({"o.applyOps": {"$exists": true}}).count(), 1);
    assert.eq(oplogColl.find({op: "i", ns: testColl.getFullName()}).count(), 0);
    // Ensure non-atomic apply ops logging produces an oplog entry for
    // each operation in the apply ops call and no record of applyOps
    // appears for these operations.
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
    assert.eq(oplogColl.find({"o.applyOps": {"$exists": true}}).count(), 1);

    // Ensure that applyOps respects the 'allowAtomic' boolean flag on CRUD operations that it would
    // have applied atomically.
    assert.commandWorked(testDB.createCollection(testColl.getName()));
    assert.commandFailedWithCode(testDB.runCommand({applyOps: [], allowAtomic: 'must be boolean'}),
                                 ErrorCodes.TypeMismatch,
                                 'allowAtomic flag must be a boolean.');
    assert.commandWorked(testDB.runCommand({
        applyOps: [
            {op: "i", ns: testColl.getFullName(), o: {_id: 3, a: "augh"}},
            {op: "i", ns: testColl.getFullName(), o: {_id: 4, a: "blah"}}
        ],
        allowAtomic: false,
    }));
    assert.eq(oplogColl.find({"o.applyOps": {"$exists": true}}).count(), 1);
    assert.eq(oplogColl.find({op: "i", ns: testColl.getFullName()}).count(), 2);

    rst.stopSet();
})();
