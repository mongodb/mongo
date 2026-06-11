// Test count and distinct on views use with different values of the allowDiskUseByDefault
// parameter.

import {checkSbeCompletelyDisabled, checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const viewsDB = conn.getDB(jsTestName());
viewsDB.largeColl.drop();

// The explicit plan cache clear below is only needed when SBE is fully enabled; under the classic
// engine the test keeps its original behavior so it still exercises the classic plan cache.
const sbeFullyEnabled = checkSbeFullyEnabled(viewsDB);

const memoryLimitMb = 1;
const largeStr = "A".repeat(1024 * 1024); // 1MB string

// Create a collection exceeding the memory limit.
for (let i = 0; i < memoryLimitMb + 1; ++i) assert.commandWorked(viewsDB.largeColl.insert({x: i, largeStr: largeStr}));

viewsDB.largeView.drop();
assert.commandWorked(viewsDB.createView("largeView", "largeColl", [{$sort: {x: -1}}]));

function testDiskUse(cmd) {
    assert.commandWorked(viewsDB.adminCommand({setParameter: 1, allowDiskUseByDefault: false}));
    assert.commandFailedWithCode(viewsDB.runCommand(cmd), ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

    assert.commandWorked(viewsDB.adminCommand({setParameter: 1, allowDiskUseByDefault: true}));
    // TODO SERVER-67035: Remove this explicit plan cache clear once 'featureFlagSbeFull' is removed.
    // Under SBE full, changing 'allowDiskUseByDefault' no longer implicitly clears the SBE plan
    // cache, so clear it explicitly; otherwise the cached plan from the no-disk run above is reused
    // and still fails. Gated on SBE full so we don't mask classic plan cache behavior.
    if (sbeFullyEnabled) {
        viewsDB.largeColl.getPlanCache().clear();
    }
    assert.commandWorked(viewsDB.runCommand(cmd));
}

// The 'count' command executes the view definition pipeline containing the '$sort' stage. This
// stage needs to spill to disk if the memory limit is reached.
assert.commandWorked(
    viewsDB.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: memoryLimitMb * 1024 * 1024}),
);

// In SBE the $sort will not cause spilling because it's only the integers being sorted on.
if (checkSbeCompletelyDisabled(viewsDB)) {
    testDiskUse({count: "largeView"});
}

// The 'distinct' command executes the view definition pipeline containing the '$sort' stage. This
// stage needs to spill to disk if the memory limit is reached.
testDiskUse({distinct: "largeView", key: "largeStr"});

MongoRunner.stopMongod(conn);
