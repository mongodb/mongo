/**
 * Tests the `ttlMonitorBackgroundOperation` option to switch the TTL monitor from being
 * non-deprioritizable to a background task.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getTotalMarkedNonDeprioritizableCount} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

const testName = jsTestName();
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Set TTL monitor sleep interval to a low value for faster testing.
            ttlMonitorSleepSecs: 1,
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(testName);
const coll = db.getCollection("ttl_coll");

// Create a TTL index that expires documents after 0 seconds.
assert.commandWorked(coll.createIndex({createdAt: 1}, {expireAfterSeconds: 0}));

function testTTLDeletion(nonDeprioritizable) {
    // Fetch the initial non-deprioritizable count.
    const initialNonDeprioritizableCount = getTotalMarkedNonDeprioritizableCount(primary);

    // Insert some documents with a createdAt time in the past.
    const now = new Date();
    const pastDate = new Date(now.getTime() - 10000);
    assert.commandWorked(
        coll.insertMany([
            {_id: 1, createdAt: pastDate},
            {_id: 2, createdAt: pastDate},
            {_id: 3, createdAt: pastDate},
        ]),
    );

    // Assert that the TTL deletion eventually happens.
    assert.soon(() => {
        return coll.countDocuments({}) === 0;
    }, "TTL deletion did not happen for all documents");

    if (nonDeprioritizable) {
        assert.gt(getTotalMarkedNonDeprioritizableCount(primary), initialNonDeprioritizableCount);
    } else {
        assert.eq(getTotalMarkedNonDeprioritizableCount(primary), initialNonDeprioritizableCount);
    }
}

// By default, TTL deletion is marked as non-deprioritizable.
testTTLDeletion(true /* nonDeprioritizable */);

// When the TTL monitor background operation is enabled, TTL deletion is marked as background.
assert.commandWorked(primary.adminCommand({setParameter: 1, ttlMonitorBackgroundOperation: true}));
testTTLDeletion(false /* nonDeprioritizable */);

rst.stopSet();
