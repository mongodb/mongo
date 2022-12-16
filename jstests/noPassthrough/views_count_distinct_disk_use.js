// Test count and distinct on views use with different values of the allowDiskUseByDefault
// parameter.

(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const viewsDB = conn.getDB(jsTestName());
viewsDB.largeColl.drop();

const memoryLimitMb = 1;
const largeStr = "A".repeat(1024 * 1024);  // 1MB string

// Create a collection exceeding the memory limit.
for (let i = 0; i < memoryLimitMb + 1; ++i)
    assert.commandWorked(viewsDB.largeColl.insert({x: i, largeStr: largeStr}));

viewsDB.largeView.drop();
assert.commandWorked(viewsDB.createView("largeView", "largeColl", [{$sort: {x: -1}}]));

function testDiskUse(cmd) {
    assert.commandWorked(viewsDB.adminCommand({setParameter: 1, allowDiskUseByDefault: false}));
    assert.commandFailedWithCode(viewsDB.runCommand(cmd),
                                 ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

    assert.commandWorked(viewsDB.adminCommand({setParameter: 1, allowDiskUseByDefault: true}));
    assert.commandWorked(viewsDB.runCommand(cmd));
}

// The 'count' command executes the view definition pipeline containing the '$sort' stage. This
// stage needs to spill to disk if the memory limit is reached.
assert.commandWorked(viewsDB.adminCommand(
    {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: memoryLimitMb * 1024 * 1024}));

// In SBE the $sort will not cause spilling because it's only the integers being sorted on.
if (!checkSBEEnabled(viewsDB)) {
    testDiskUse({count: "largeView"});
}

// The 'distinct' command executes the view definition pipeline containing the '$sort' stage. This
// stage needs to spill to disk if the memory limit is reached.
testDiskUse({distinct: "largeView", key: "largeStr"});

MongoRunner.stopMongod(conn);
})();
